#include "ytec/winpeapp/direct_image_restore.h"

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
constexpr std::uint64_t kPartitionOffset = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionSize = 2ULL * 1024ULL * 1024ULL;

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
  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskSize;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t,
      const std::span<const std::byte>) override {
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t,
      const std::size_t length) const override {
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(length, std::byte{0}));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    return ytec::clonecore::success_status();
  }
};

ytec::diskmodel::DiskInfo target_disk(
    std::wstring bus = L"SATA",
    const bool removable = false) {
  return ytec::diskmodel::DiskInfo{
      .disk_number = 6U,
      .device_path = L"\\\\.\\PhysicalDrive6",
      .device_instance_id = L"SYNTHETIC\\PE-TARGET-6",
      .model = L"Synthetic PE Target SSD",
      .size_bytes = kDiskSize,
      .sector_count = kDiskSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = std::move(bus),
      .serial_suffix = "PT06",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .offline = false,
      .read_only = false,
      .removable = removable,
  };
}

ytec::clonecore::StableDiskIdentity stable(
    const ytec::diskmodel::DiskInfo& disk) {
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "synthetic identity must build");
  return identity.take_value();
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
  const auto model = ytec::imageformat::hash_tsumugi_source_model_v1(
      L"Synthetic PE Source SSD");
  const auto serial = ytec::imageformat::hash_tsumugi_source_serial_v1(
      "PS01", L"SYNTHETIC\\PE-SOURCE-1");
  check(model.has_value() && serial.has_value(), "source hashes must build");
  image.manifest.source_model_hash = model.value();
  image.manifest.source_serial_hash = serial.value();
  image.manifest.source_state_hash.fill(std::byte{0x41});
  image.manifest.partitions.push_back({
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .flags = ytec::imageformat::TsumugiManifestPartitionFlags::selected,
      .source_offset = 1024ULL * 1024ULL,
      .source_size = kPartitionSize,
      .used_bytes = kPartitionSize,
      .minimum_target_bytes = kPartitionSize,
      .planned_target_bytes = kPartitionSize,
      .payload_logical_offset = 1024ULL * 1024ULL,
      .payload_logical_length = kPartitionSize,
  });
  image.container.global_hash.fill(std::byte{0x62});
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
  bool active_media{};
  bool active_query_fails{};
  bool engine_fails{};
  bool engine_called{};
  bool individual_engine_called{};
  bool engine_saw_usb_token{};
  bool signature_collector_called{};
  bool final_reidentify_drift{};
  std::vector<std::uint32_t> engine_signatures;
  std::size_t reidentify_calls{};

  ytec::winpeapp::DirectImageRestoreRequest request() const {
    const auto layout =
        ytec::winpeapp::hash_direct_image_restore_target_layout(initial);
    check(layout.has_value(), "target layout hash must build");
    return ytec::winpeapp::DirectImageRestoreRequest{
        .image = {
            .image_path = L"E:\\reviewed.tsumugi",
            .storage_file_system = ytec::imageformat::
                TsumugiImageStorageFileSystem::ntfs,
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

  ytec::winpeapp::DirectImageRestoreDependencies dependencies() {
    return {
        .is_active_rescue_media =
            [&](const ytec::clonecore::StableDiskIdentity& expected,
                const ytec::clonecore::TargetConfirmation&) {
              events.push_back("active-media");
              if (active_query_fails) {
                return ytec::clonecore::Result<bool>::failure(
                    injected_error(L"合成PE起動媒体照合"));
              }
              check(expected.disk_number == initial.disk_number,
                    "active-media query must receive stable selection");
              return ytec::clonecore::Result<bool>::success(active_media);
            },
        .physical = {
            .verify_image =
                [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                    const ytec::clonecore::DiskOperationCallbacks&) {
                  events.push_back("verify");
                  return ytec::clonecore::Result<
                      ytec::imageformat::TsumugiVerifiedImage>::success(
                      image);
                },
            .reidentify_target =
                [&](const ytec::clonecore::StableDiskIdentity&,
                    const ytec::clonecore::TargetConfirmation&) {
                  events.push_back("reidentify");
                  ++reidentify_calls;
                  auto current =
                      reidentify_calls == 1U ? initial : opened;
                  if (final_reidentify_drift && reidentify_calls > 1U) {
                    current.partition_style =
                        ytec::diskmodel::PartitionStyle::gpt;
                    current.partitions.push_back({
                        .number = 9U,
                        .offset_bytes = 6U * 1024U * 1024U,
                        .size_bytes = 1024U * 1024U,
                        .style = ytec::diskmodel::PartitionStyle::gpt,
                        .type = L"Basic Data",
                    });
                  }
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
                      {0x11223344U, 0x55667788U});
                },
            .make_connection_token = [&] {
              events.push_back("connection-token");
              ytec::imageformat::Sha256Digest token{};
              token.fill(std::byte{0x7B});
              return ytec::clonecore::Result<
                  ytec::imageformat::Sha256Digest>::success(token);
            },
            .execute_engine =
                [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                    const ytec::imageformat::TsumugiVerifiedImage&,
                    const ytec::imageformat::TsumugiRestoreDiskIdentity&
                        identity,
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
                  engine_signatures.assign(
                      signatures.begin(), signatures.end());
                  check(handle.target != nullptr,
                        "engine must own the exact opened handle");
                  auto revalidated = revalidate_locked_target();
                  if (!revalidated) {
                    return ytec::clonecore::Result<
                        ytec::imageformat::TsumugiRestoreReport>::failure(
                        revalidated.error());
                  }
                  if (engine_fails) {
                    return ytec::clonecore::Result<
                        ytec::imageformat::TsumugiRestoreReport>::failure(
                        injected_error(L"合成PE復元Engine"));
                  }
                  return ytec::clonecore::Result<
                      ytec::imageformat::TsumugiRestoreReport>::success(
                      successful_engine_report());
                },
            .execute_individual_partition_engine =
                [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                    const ytec::imageformat::TsumugiVerifiedImage&,
                    const ytec::imageformat::TsumugiRestoreDiskIdentity&,
                    const ytec::imageformat::
                        TsumugiPhysicalIndividualPartitionRestoreSelection&
                            selection,
                    ytec::diskmodel::PhysicalTargetHandle handle,
                    const ytec::imageformat::
                        TsumugiPhysicalRestoreLockedTargetRevalidator&
                            revalidate_locked_target,
                    const ytec::clonecore::DiskOperationCallbacks&) {
                  events.push_back("individual-engine");
                  individual_engine_called = true;
                  check(selection.source_table_index == 1U,
                        "individual engine must receive reviewed source partition");
                  check(handle.target != nullptr,
                        "individual engine must own the exact opened handle");
                  auto revalidated = revalidate_locked_target();
                  if (!revalidated) {
                    return ytec::clonecore::Result<
                        ytec::imageformat::TsumugiRestoreReport>::failure(
                        revalidated.error());
                  }
                  return ytec::clonecore::Result<
                      ytec::imageformat::TsumugiRestoreReport>::success(
                      successful_engine_report());
                },
        },
    };
  }
};

void exact_restore_is_direct_and_keeps_target_offline() {
  Fixture fixture;
  const auto result = ytec::winpeapp::execute_direct_image_restore(
      fixture.request(), fixture.dependencies());
  check(result.has_value(), "PE exact direct restore must succeed");
  check(
      fixture.events == std::vector<std::string>{
          "active-media", "verify", "reidentify", "offline", "open",
          "engine", "reidentify", "offline"},
      "boot-media check and full image verification must precede writes");
  check(result.value().active_rescue_media_checked &&
            result.value().direct_execution_only &&
            result.value().physical.target_left_offline &&
            fixture.offline_requests == std::vector<bool>{true, true},
        "direct restore must create no job and preserve offline state");
}

void individual_restore_dispatches_only_the_reviewed_partition_engine() {
  Fixture fixture;
  fixture.image.manifest.partitions.front().flags =
      ytec::imageformat::TsumugiManifestPartitionFlags::selected |
      ytec::imageformat::TsumugiManifestPartitionFlags::contains_windows;
  fixture.initial.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  fixture.initial.partitions.push_back({
      .number = 3U,
      .offset_bytes = kPartitionOffset,
      .size_bytes = kPartitionSize,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"Basic Data",
      .name = L"Existing target",
  });
  fixture.opened = fixture.initial;
  auto request = fixture.request();
  request.individual_partition = ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalExistingPartitionRestoreSelection{
                  .target_table_index = 1U,
                  .target_partition_number = 3U,
                  .target_offset = kPartitionOffset,
                  .target_size = kPartitionSize,
              },
      };
  const auto result = ytec::winpeapp::execute_direct_image_restore(
      request, fixture.dependencies());
  check(result.has_value() && fixture.individual_engine_called &&
            !fixture.engine_called &&
            result.value().physical.boot_repair_offer_required &&
            fixture.offline_requests == std::vector<bool>{true, true},
        "PE individual restore must dispatch only the reviewed partition engine, stay offline, and offer separate Windows boot repair");
}

void unallocated_individual_restore_is_reviewed_and_dispatched() {
  Fixture fixture;
  fixture.initial.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  fixture.opened = fixture.initial;
  auto request = fixture.request();
  request.individual_partition = ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalUnallocatedRestoreSelection{
                  .target_offset = 4ULL * 1024ULL * 1024ULL,
                  .target_size = kPartitionSize,
              },
      };
  const auto result = ytec::winpeapp::execute_direct_image_restore(
      request, fixture.dependencies());
  check(result.has_value() &&
            fixture.offline_requests == std::vector<bool>{true, true} &&
            !fixture.engine_called && fixture.individual_engine_called,
        "reviewed unallocated placement must dispatch only the preserving individual engine");
}

void active_rescue_media_or_uncertain_query_fails_before_image_or_disk_io() {
  Fixture active;
  active.active_media = true;
  const auto blocked = ytec::winpeapp::execute_direct_image_restore(
      active.request(), active.dependencies());
  check(!blocked.has_value() &&
            active.events == std::vector<std::string>{"active-media"} &&
            active.offline_requests.empty() && !active.engine_called,
        "the currently booted PE USB must be excluded before image/disk I/O");

  Fixture uncertain;
  uncertain.active_query_fails = true;
  const auto failed = ytec::winpeapp::execute_direct_image_restore(
      uncertain.request(), uncertain.dependencies());
  check(!failed.has_value() &&
            uncertain.events == std::vector<std::string>{"active-media"} &&
            uncertain.offline_requests.empty(),
        "uncertain boot-media identity must fail closed");

  auto missing_dependencies = uncertain.dependencies();
  missing_dependencies.is_active_rescue_media = {};
  uncertain.events.clear();
  const auto missing = ytec::winpeapp::execute_direct_image_restore(
      uncertain.request(), missing_dependencies);
  check(!missing.has_value() && uncertain.events.empty(),
        "missing boot-media resolver must perform no environment call");
}

void confirmation_must_be_uppercase_ok() {
  Fixture fixture;
  auto request = fixture.request();
  request.confirmation.typed_token = L"ok";
  const auto result = ytec::winpeapp::execute_direct_image_restore(
      request, fixture.dependencies());
  check(!result.has_value() && fixture.events.empty() &&
            fixture.offline_requests.empty(),
        "lowercase confirmation must stop before every resolver and write");
}

void source_target_and_unsupported_classes_fail_before_offline() {
  Fixture source;
  source.image.manifest.source_model_hash =
      ytec::imageformat::hash_tsumugi_source_model_v1(
          source.initial.model).value();
  source.image.manifest.source_serial_hash =
      ytec::imageformat::hash_tsumugi_source_serial_v1(
          source.initial.serial_suffix,
          source.initial.device_instance_id).value();
  const auto same_source = ytec::winpeapp::execute_direct_image_restore(
      source.request(), source.dependencies());
  check(!same_source.has_value() && source.offline_requests.empty(),
        "the image's original source disk must never be overwritten");

  Fixture usb_memory;
  usb_memory.initial.bus_type = L"USB";
  usb_memory.initial.removable = true;
  usb_memory.opened = usb_memory.initial;
  const auto usb = ytec::winpeapp::execute_direct_image_restore(
      usb_memory.request(), usb_memory.dependencies());
  check(!usb.has_value() && usb_memory.offline_requests.empty(),
        "USB memory must not be a physical restore target");

  Fixture system;
  system.initial.is_system_disk = true;
  system.opened = system.initial;
  const auto running = ytec::winpeapp::execute_direct_image_restore(
      system.request(), system.dependencies());
  check(!running.has_value() && system.offline_requests.empty(),
        "the running system disk must fail before offline transition");
}

void shrink_and_review_drift_remain_fail_closed() {
  Fixture shrink;
  shrink.image.manifest.mode =
      ytec::imageformat::TsumugiManifestMode::shrink;
  const auto unsupported = ytec::winpeapp::execute_direct_image_restore(
      shrink.request(), shrink.dependencies());
  check(!unsupported.has_value() && shrink.offline_requests.empty(),
        "unconnected shrink placement must perform no target mutation");

  Fixture before;
  auto request = before.request();
  before.initial.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  before.initial.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = 4U * 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"Basic Data",
  });
  before.opened = before.initial;
  const auto changed_before = ytec::winpeapp::execute_direct_image_restore(
      request, before.dependencies());
  check(!changed_before.has_value() && before.offline_requests.empty(),
        "layout drift after review must stop before offline");

  Fixture after;
  request = after.request();
  after.opened.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  after.opened.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = 4U * 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"Basic Data",
  });
  const auto changed_after = ytec::winpeapp::execute_direct_image_restore(
      request, after.dependencies());
  check(!changed_after.has_value() && !after.engine_called &&
            after.offline_requests == std::vector<bool>{true, true},
        "opened-handle layout drift must stay offline and skip engine");

  Fixture final;
  final.final_reidentify_drift = true;
  const auto changed_final = ytec::winpeapp::execute_direct_image_restore(
      final.request(), final.dependencies());
  check(!changed_final.has_value() && final.engine_called &&
            final.offline_requests == std::vector<bool>{true, true},
        "layout drift during final image verification must stop before writes and stay offline");
}

void usb_epoch_mbr_signatures_and_failure_offline_are_enforced() {
  Fixture usb;
  usb.initial.bus_type = L"USB";
  usb.opened = usb.initial;
  const auto usb_result = ytec::winpeapp::execute_direct_image_restore(
      usb.request(), usb.dependencies());
  check(usb_result.has_value() && usb.engine_saw_usb_token,
        "USB disk restore must bind a nonzero same-connection epoch");

  Fixture mbr;
  mbr.image = verified_image(
      ytec::imageformat::TsumugiManifestPartitionStyle::mbr);
  const auto mbr_result = ytec::winpeapp::execute_direct_image_restore(
      mbr.request(), mbr.dependencies());
  check(mbr_result.has_value() && mbr.signature_collector_called &&
            mbr.engine_signatures ==
                std::vector<std::uint32_t>{0x11223344U, 0x55667788U},
        "MBR restore must collect connected signatures before offline");
  const auto signature = std::find(
      mbr.events.begin(), mbr.events.end(), "signatures");
  const auto offline = std::find(
      mbr.events.begin(), mbr.events.end(), "offline");
  check(signature != mbr.events.end() && offline != mbr.events.end() &&
            signature < offline,
        "MBR signature collision inventory must precede target mutation");

  Fixture failure;
  failure.engine_fails = true;
  const auto failed = ytec::winpeapp::execute_direct_image_restore(
      failure.request(), failure.dependencies());
  check(!failed.has_value() &&
            failure.offline_requests == std::vector<bool>{true, true},
        "failed PE restore must reassert offline state");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"exact_restore_is_direct_and_keeps_target_offline",
       exact_restore_is_direct_and_keeps_target_offline},
      {"individual_restore_dispatches_only_the_reviewed_partition_engine",
       individual_restore_dispatches_only_the_reviewed_partition_engine},
      {"unallocated_individual_restore_is_reviewed_and_dispatched",
       unallocated_individual_restore_is_reviewed_and_dispatched},
      {"active_rescue_media_or_uncertain_query_fails_before_image_or_disk_io",
       active_rescue_media_or_uncertain_query_fails_before_image_or_disk_io},
      {"confirmation_must_be_uppercase_ok",
       confirmation_must_be_uppercase_ok},
      {"source_target_and_unsupported_classes_fail_before_offline",
       source_target_and_unsupported_classes_fail_before_offline},
      {"shrink_and_review_drift_remain_fail_closed",
       shrink_and_review_drift_remain_fail_closed},
      {"usb_epoch_mbr_signatures_and_failure_offline_are_enforced",
       usb_epoch_mbr_signatures_and_failure_offline_are_enforced},
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

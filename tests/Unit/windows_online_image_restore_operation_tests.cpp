#include "ytec/windowsapp/online_image_restore_operation.h"
#include "ytec/windowsapp/online_image_create.h"

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
constexpr std::uint64_t kPayloadBytes = 4096U;

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
      std::uint64_t,
      std::span<const std::byte>) override {
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      std::uint64_t,
      const std::size_t length) const override {
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(length));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    return ytec::clonecore::success_status();
  }
};

ytec::diskmodel::DiskInfo target_disk() {
  return ytec::diskmodel::DiskInfo{
      .disk_number = 5U,
      .device_path = L"\\\\.\\PhysicalDrive5",
      .device_instance_id = L"SYNTHETIC\\RESTORE-TARGET-5",
      .model = L"Synthetic Restore Target",
      .size_bytes = kDiskSize,
      .sector_count = kDiskSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "DST0005",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .offline = false,
      .read_only = false,
      .removable = false,
  };
}

ytec::clonecore::StableDiskIdentity stable(
    const ytec::diskmodel::DiskInfo& disk) {
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "synthetic identity must build");
  return identity.take_value();
}

ytec::imageformat::TsumugiVerifiedImage verified_image() {
  ytec::imageformat::TsumugiVerifiedImage image;
  image.container.global_hash.fill(std::byte{0x71});
  image.manifest.mode = ytec::imageformat::TsumugiManifestMode::exact;
  image.manifest.partition_style =
      ytec::imageformat::TsumugiManifestPartitionStyle::gpt;
  image.manifest.source_disk_size = 8ULL * 1024ULL * 1024ULL;
  image.manifest.logical_sector_size = kSectorSize;
  image.manifest.physical_sector_size = 4096U;
  image.manifest.source_state_hash.fill(std::byte{0x31});
  auto model = ytec::windowsapp::hash_tsumugi_source_model(
      L"Synthetic Source");
  auto serial = ytec::windowsapp::hash_tsumugi_source_serial(
      "SRC0001", L"SYNTHETIC\\SOURCE-1");
  check(model.has_value() && serial.has_value(), "source hashes must build");
  image.manifest.source_model_hash = model.take_value();
  image.manifest.source_serial_hash = serial.take_value();
  image.manifest.partitions.push_back({
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .flags = ytec::imageformat::TsumugiManifestPartitionFlags::selected,
      .source_offset = 1024U * 1024U,
      .source_size = kPayloadBytes,
      .used_bytes = kPayloadBytes,
      .minimum_target_bytes = kPayloadBytes,
      .planned_target_bytes = kPayloadBytes,
      .payload_logical_offset = 1024U * 1024U,
      .payload_logical_length = kPayloadBytes,
  });
  return image;
}

ytec::operationcore::OperationId operation_id() {
  ytec::operationcore::OperationId id{};
  for (std::size_t index = 0U; index < id.size(); ++index) {
    id[index] = static_cast<std::byte>(index + 1U);
  }
  return id;
}

struct Fixture final {
  ytec::diskmodel::DiskInfo reviewed = target_disk();
  ytec::diskmodel::DiskInfo observed = reviewed;
  ytec::diskmodel::DiskInfo opened = reviewed;
  ytec::imageformat::TsumugiVerifiedImage verified = verified_image();
  std::vector<std::string> events;
  std::vector<bool> offline;
  bool omit_final_proof{};
  std::size_t reidentify_calls{};

  ytec::windowsapp::OnlineImageRestoreOperationRequest request(
      std::wstring token = L"OK") const {
    auto layout =
        ytec::windowsapp::hash_online_image_restore_target_layout(reviewed);
    check(layout.has_value(), "reviewed layout hash must build");
    ytec::windowsapp::TsumugiRestoreImagePreflightReport preflight{
        .canonical_path = L"D:\\verified.tsumugi",
        .storage_file_system = ytec::imageformat::
            TsumugiImageStorageFileSystem::ntfs,
        .image_length = 8192U,
        .global_hash = verified.container.global_hash,
        .manifest = verified.manifest,
        .complete_container_verified = true,
        .metadata_verified = true,
        .restore_layout_verified = true,
    };
    return {
        .reviewed_image = std::move(preflight),
        .reviewed_target = reviewed,
        .restore = {
            .image = {
                .image_path = L"D:\\verified.tsumugi",
                .storage_file_system = ytec::imageformat::
                    TsumugiImageStorageFileSystem::ntfs,
            },
            .expected_image_global_hash = verified.container.global_hash,
            .expected_source_state_hash = verified.manifest.source_state_hash,
            .expected_target = stable(reviewed),
            .expected_target_layout_hash = layout.take_value(),
            .confirmation = {
                .first_step_acknowledged = true,
                .typed_token = std::move(token),
            },
            .administrator = true,
        },
        .operation_id = operation_id(),
    };
  }

  ytec::windowsapp::OnlineImageRestoreDependencies dependencies() {
    return {
        .verify_image =
            [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                const ytec::clonecore::DiskOperationCallbacks&) {
              events.push_back("verify-image");
              return ytec::clonecore::Result<
                  ytec::imageformat::TsumugiVerifiedImage>::success(verified);
            },
        .reidentify_target =
            [&](const ytec::clonecore::StableDiskIdentity&,
                const ytec::clonecore::TargetConfirmation&) {
              events.push_back("reidentify");
              ++reidentify_calls;
              const auto& current =
                  reidentify_calls >= 3U ? opened : observed;
              return ytec::clonecore::Result<
                  ytec::diskmodel::ReidentifiedPhysicalTarget>::success({
                  .target = current,
                  .target_identity = stable(current),
              });
            },
        .set_target_offline =
            [&](const ytec::clonecore::StableDiskIdentity&,
                const ytec::clonecore::TargetConfirmation&,
                const bool value) {
              events.push_back("offline");
              offline.push_back(value);
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
            [](const ytec::clonecore::StableDiskIdentity&) {
              return ytec::clonecore::Result<
                  std::vector<std::uint32_t>>::success({});
            },
        .make_connection_token = [] {
          ytec::imageformat::Sha256Digest token{};
          token.fill(std::byte{0x44});
          return ytec::clonecore::Result<
              ytec::imageformat::Sha256Digest>::success(token);
        },
        .execute_engine =
            [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                const ytec::imageformat::TsumugiVerifiedImage&,
                const ytec::imageformat::TsumugiRestoreDiskIdentity&,
                ytec::diskmodel::PhysicalTargetHandle,
                std::span<const std::uint32_t>,
                const ytec::imageformat::
                    TsumugiPhysicalRestoreLockedTargetRevalidator&
                        revalidate_locked_target,
                const ytec::clonecore::DiskOperationCallbacks&) {
              events.push_back("engine");
              auto revalidated = revalidate_locked_target();
              if (!revalidated) {
                return ytec::clonecore::Result<
                    ytec::imageformat::TsumugiRestoreReport>::failure(
                    revalidated.error());
              }
              return ytec::clonecore::Result<
                  ytec::imageformat::TsumugiRestoreReport>::success({
                  .written_logical_bytes = kPayloadBytes,
                  .written_chunk_count = 1U,
                  .callbacks_started_after_complete_verification = true,
                  .image_matched_prepared_plan = true,
                  .target_reidentified_before_write = true,
                  .all_writes_read_back_verified = true,
                  .final_layout_committed = !omit_final_proof,
              });
            },
        .execute_individual_partition_engine =
            [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                const ytec::imageformat::TsumugiVerifiedImage&,
                const ytec::imageformat::TsumugiRestoreDiskIdentity&,
                const ytec::imageformat::
                    TsumugiPhysicalIndividualPartitionRestoreSelection&,
                ytec::diskmodel::PhysicalTargetHandle,
                const ytec::imageformat::
                    TsumugiPhysicalRestoreLockedTargetRevalidator&
                        revalidate_locked_target,
                const ytec::clonecore::DiskOperationCallbacks&) {
              events.push_back("individual-engine");
              auto revalidated = revalidate_locked_target();
              if (!revalidated) {
                return ytec::clonecore::Result<
                    ytec::imageformat::TsumugiRestoreReport>::failure(
                    revalidated.error());
              }
              return ytec::clonecore::Result<
                  ytec::imageformat::TsumugiRestoreReport>::success({
                  .written_logical_bytes = kPayloadBytes,
                  .written_chunk_count = 1U,
                  .callbacks_started_after_complete_verification = true,
                  .image_matched_prepared_plan = true,
                  .target_reidentified_before_write = true,
                  .all_writes_read_back_verified = true,
                  .final_layout_committed = true,
              });
            },
    };
  }
};

void test_plan_and_successful_lifecycle_are_bound() {
  Fixture fixture;
  const auto request = fixture.request();
  const auto plan =
      ytec::windowsapp::make_online_image_restore_operation_plan(request);
  check(
      plan.has_value() &&
          plan.value().kind ==
              ytec::operationcore::OperationKind::image_restore &&
          plan.value().expected_work_bytes == kPayloadBytes &&
          plan.value().target.has_value(),
      "reviewed image and target must produce one immutable restore plan");

  const auto result =
      ytec::windowsapp::execute_online_image_restore_operation(
          request, fixture.dependencies());
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          result.value().restore.has_value() &&
          result.value().lifecycle.processed_work_bytes == kPayloadBytes &&
          fixture.offline == std::vector<bool>{true, true},
      "restore must complete only through OperationPlan and verified engine");
  check(
      fixture.events.size() >= 4U && fixture.events[0] == "reidentify" &&
          fixture.events[1] == "verify-image",
      "OperationCore re-identification must occur before image/controller execution");
}

void test_exact_ok_is_enforced_before_offline() {
  Fixture fixture;
  const auto result =
      ytec::windowsapp::execute_online_image_restore_operation(
          fixture.request(L"ok"), fixture.dependencies());
  check(
      result.has_value() &&
          result.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::awaiting_confirmation &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          fixture.offline.empty(),
      "only exact uppercase OK may cross the destructive boundary");
}

void test_image_or_target_drift_stops_before_mutation() {
  Fixture image;
  const auto request = image.request();
  image.verified.container.global_hash[0] ^= std::byte{0x01};
  const auto changed_image =
      ytec::windowsapp::execute_online_image_restore_operation(
          request, image.dependencies());
  check(
      changed_image.has_value() &&
          changed_image.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::executing &&
          image.offline.empty(),
      "same-path image replacement must stop before target mutation");

  Fixture target;
  const auto target_request = target.request();
  target.observed.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  target.observed.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
  });
  const auto changed_target =
      ytec::windowsapp::execute_online_image_restore_operation(
          target_request, target.dependencies());
  check(
      changed_target.has_value() &&
          changed_target.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::reidentifying &&
          target.offline.empty(),
      "target layout drift must stop at OperationCore re-identification");
}

void test_missing_final_proof_is_not_completed() {
  Fixture fixture;
  fixture.omit_final_proof = true;
  const auto result =
      ytec::windowsapp::execute_online_image_restore_operation(
          fixture.request(), fixture.dependencies());
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          result.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::executing &&
          fixture.offline == std::vector<bool>{true, true},
      "missing final layout proof must fail and preserve offline state");
}

void test_individual_partition_selection_is_bound_and_dispatched() {
  Fixture fixture;
  const ytec::diskmodel::PartitionInfo existing{
      .number = 3U,
      .offset_bytes = 2ULL * 1024ULL * 1024ULL,
      .size_bytes = 2ULL * 1024ULL * 1024ULL,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
      .name = L"Existing data",
  };
  fixture.reviewed.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  fixture.reviewed.partitions.push_back(existing);
  fixture.observed = fixture.reviewed;
  fixture.opened = fixture.reviewed;
  auto request = fixture.request();
  request.restore.individual_partition = ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalExistingPartitionRestoreSelection{
                  .target_table_index = 1U,
                  .target_partition_number = existing.number,
                  .target_offset = existing.offset_bytes,
                  .target_size = existing.size_bytes,
              },
      };
  const auto plan =
      ytec::windowsapp::make_online_image_restore_operation_plan(request);
  check(plan.has_value() &&
            plan.value().expected_work_bytes == kPayloadBytes,
        "individual plan must bind only the selected source payload");
  const auto result =
      ytec::windowsapp::execute_online_image_restore_operation(
          request, fixture.dependencies());
  check(result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::completed &&
            std::find(
                fixture.events.begin(),
                fixture.events.end(),
                "individual-engine") != fixture.events.end() &&
            std::find(
                fixture.events.begin(), fixture.events.end(), "engine") ==
                fixture.events.end(),
        "reviewed individual placement must dispatch only its dedicated engine");
}

void test_unallocated_partition_selection_is_bound_and_dispatched() {
  Fixture fixture;
  fixture.reviewed.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  fixture.observed = fixture.reviewed;
  fixture.opened = fixture.reviewed;
  auto request = fixture.request();
  request.restore.individual_partition = ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalUnallocatedRestoreSelection{
                  .target_offset = 4ULL * 1024ULL * 1024ULL,
                  .target_size = kPayloadBytes,
              },
      };
  const auto plan =
      ytec::windowsapp::make_online_image_restore_operation_plan(request);
  check(plan.has_value() &&
            plan.value().expected_work_bytes == kPayloadBytes,
        "unallocated plan must bind the exact source and reviewed gap");
  const auto result =
      ytec::windowsapp::execute_online_image_restore_operation(
          request, fixture.dependencies());
  check(result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::completed &&
            std::find(
                fixture.events.begin(),
                fixture.events.end(),
                "individual-engine") != fixture.events.end() &&
            std::find(
                fixture.events.begin(), fixture.events.end(), "engine") ==
                fixture.events.end(),
        "unallocated placement must dispatch only its preserving individual engine");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"plan_and_successful_lifecycle_are_bound",
       test_plan_and_successful_lifecycle_are_bound},
      {"exact_ok_is_enforced_before_offline",
       test_exact_ok_is_enforced_before_offline},
      {"image_or_target_drift_stops_before_mutation",
       test_image_or_target_drift_stops_before_mutation},
      {"missing_final_proof_is_not_completed",
       test_missing_final_proof_is_not_completed},
      {"individual_partition_selection_is_bound_and_dispatched",
       test_individual_partition_selection_is_bound_and_dispatched},
      {"unallocated_partition_selection_is_bound_and_dispatched",
       test_unallocated_partition_selection_is_bound_and_dispatched},
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

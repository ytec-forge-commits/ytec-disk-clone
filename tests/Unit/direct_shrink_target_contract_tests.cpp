#include "ytec/directshrink/target_contract.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Digest>
Digest digest(const unsigned char seed) {
  Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>(seed + index);
  }
  return value;
}

ytec::operationcore::OperationId operation_id() {
  return digest<ytec::operationcore::OperationId>(0x10U);
}

ytec::clonecore::StableDiskIdentity source_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 1U,
      .model = L"Synthetic source",
      .size_bytes = 8ULL * kGiB,
      .logical_sector_size = 512U,
      .serial_suffix = "SOURCE-01",
      .device_instance_id = L"SYNTHETIC\\SOURCE",
      .is_system_disk = true,
  };
}

ytec::clonecore::StableDiskIdentity target_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 2U,
      .model = L"Synthetic target",
      .size_bytes = 6ULL * kGiB,
      .logical_sector_size = 512U,
      .serial_suffix = "TARGET-02",
      .device_instance_id = L"SYNTHETIC\\TARGET",
      .is_system_disk = false,
  };
}

ytec::directshrink::PartitionTask generated_task(
    const ytec::directshrink::PartitionTaskKind kind,
    const std::uint32_t target_number,
    const ytec::migrationcore::MigrationPartitionRole role,
    const std::uint64_t offset,
    const std::uint64_t size) {
  return ytec::directshrink::PartitionTask{
      .kind = kind,
      .target_number = target_number,
      .role = role,
      .target_offset_bytes = offset,
      .construction_size_bytes = size,
      .target_size_bytes = size,
  };
}

ytec::directshrink::PartitionTask archive_task(
    const std::uint32_t target_number,
    const std::uint32_t source_table_index,
    const ytec::migrationcore::MigrationPartitionRole role,
    const std::uint64_t source_offset,
    const std::uint64_t source_size,
    const std::uint64_t used,
    const std::uint64_t target_offset,
    const std::uint64_t target_size) {
  return ytec::directshrink::PartitionTask{
      .kind = ytec::directshrink::PartitionTaskKind::apply_ntfs_wim,
      .target_number = target_number,
      .source_table_index = source_table_index,
      .role = role,
      .source_offset_bytes = source_offset,
      .target_offset_bytes = target_offset,
      .construction_size_bytes = target_size,
      .target_size_bytes = target_size,
      .source_size_bytes = source_size,
      .source_used_bytes = used,
      .original_volume_guid_path =
          L"\\\\?\\Volume{00000000-0000-0000-0000-00000000000" +
          std::to_wstring(source_table_index) + L"}\\",
      .archive_upper_bound_bytes = 1ULL * kGiB,
  };
}

ytec::directshrink::SourcePartitionMapping generated_mapping(
    const std::uint32_t source_table_index,
    const std::uint32_t target_number,
    const ytec::migrationcore::MigrationPartitionRole role) {
  return ytec::directshrink::SourcePartitionMapping{
      .source_table_index = source_table_index,
      .role = role,
      .disposition = ytec::directshrink::SourcePartitionDisposition::
          recreated_as_generated_system_partition,
      .target_number = target_number,
      .requested = false,
      .selected = true,
      .required = true,
  };
}

ytec::directshrink::SourcePartitionMapping transferred_mapping(
    const std::uint32_t source_table_index,
    const std::uint32_t target_number,
    const ytec::migrationcore::MigrationPartitionRole role,
    const bool required) {
  return ytec::directshrink::SourcePartitionMapping{
      .source_table_index = source_table_index,
      .role = role,
      .disposition = ytec::directshrink::SourcePartitionDisposition::
          transferred_to_target,
      .target_number = target_number,
      .requested = !required ||
          role == ytec::migrationcore::MigrationPartitionRole::windows,
      .selected = true,
      .required = required,
  };
}

ytec::directshrink::TargetPlanData valid_gpt_plan_data(
    const bool empty_data = false,
    const bool grow_empty_data = false) {
  using namespace ytec;
  const auto source = source_identity();
  const auto target = target_identity();
  directshrink::TargetPlanData data{
      .operation_plan = operationcore::OperationPlan{
          .schema_version = operationcore::kOperationPlanSchemaVersion,
          .operation_id = operation_id(),
          .kind = operationcore::OperationKind::clone,
          .environment = operationcore::OperationEnvironment::winpe,
          .source = source,
          .target = target,
          .expected_work_bytes = 3728ULL * kMiB,
          .immutable_payload_hash =
              digest<operationcore::Sha256Digest>(0x20U),
      },
      .expected_source = source,
      .expected_target = target,
      .expected_source_layout_hash =
          digest<imageformat::Sha256Digest>(0x30U),
      .expected_target_layout_hash =
          digest<imageformat::Sha256Digest>(0x40U),
      .source_partition_snapshot_hash =
          digest<imageformat::Sha256Digest>(0x50U),
      .source_partition_style = migrationcore::MigrationPartitionStyle::gpt,
      .partition_style = migrationcore::MigrationPartitionStyle::gpt,
      .partition_style_choice =
          migrationcore::DirectClonePartitionStyleChoice::preserve,
      .surplus_allocation =
          migrationcore::ShrinkSurplusAllocation::leave_unallocated,
      .checkpoint_offset_bytes = directshrink::kCheckpointOffsetBytes,
      .staging = directshrink::TargetOwnedStagingPlan{
          .offset_bytes = 3729ULL * kMiB,
          .length_bytes = 1088ULL * kMiB,
          .control_reserve_bytes =
              directshrink::kStagingControlReserveBytes,
          .archive_offset_bytes = 3793ULL * kMiB,
          .archive_capacity_bytes = 1ULL * kGiB,
      },
      .final_layout_hash = digest<imageformat::Sha256Digest>(0x60U),
      .boot_finalization_required = true,
      .target_is_active_rescue_media = false,
      .archive_task_count = empty_data ? 2U : 3U,
      .maximum_archive_upper_bound_bytes = 1ULL * kGiB,
      .ntfs_extension_task_count = 0U,
  };
  data.tasks = {
      generated_task(
          directshrink::PartitionTaskKind::recreate_efi_system,
          1U,
          migrationcore::MigrationPartitionRole::efi_system,
          1ULL * kMiB,
          128ULL * kMiB),
      generated_task(
          directshrink::PartitionTaskKind::recreate_microsoft_reserved,
          2U,
          migrationcore::MigrationPartitionRole::microsoft_reserved,
          129ULL * kMiB,
          16ULL * kMiB),
      archive_task(
          3U,
          3U,
          migrationcore::MigrationPartitionRole::windows,
          200ULL * kMiB,
          4ULL * kGiB,
          1ULL * kGiB,
          145ULL * kMiB,
          2ULL * kGiB),
      archive_task(
          4U,
          4U,
          migrationcore::MigrationPartitionRole::recovery,
          4300ULL * kMiB,
          512ULL * kMiB,
          256ULL * kMiB,
          2193ULL * kMiB,
          512ULL * kMiB),
      archive_task(
          5U,
          5U,
          migrationcore::MigrationPartitionRole::data,
          4812ULL * kMiB,
          2ULL * kGiB,
          128ULL * kMiB,
          2705ULL * kMiB,
          1ULL * kGiB),
  };
  if (empty_data) {
    auto& task = data.tasks.back();
    task.kind = directshrink::PartitionTaskKind::create_empty_ntfs;
    task.source_used_bytes = 0U;
    task.original_volume_guid_path.clear();
    task.archive_upper_bound_bytes = 0U;
  }
  if (grow_empty_data) {
    auto& task = data.tasks.back();
    task.construction_size_bytes = 960ULL * kMiB;
    task.target_size_bytes = 2ULL * kGiB;
    data.operation_plan.expected_work_bytes = 4752ULL * kMiB;
    data.surplus_allocation =
        migrationcore::ShrinkSurplusAllocation::selected_data_partition;
    data.surplus_target_source_table_index = 5U;
    data.staging = directshrink::TargetOwnedStagingPlan{
        .offset_bytes = 3665ULL * kMiB,
        .length_bytes = 1088ULL * kMiB,
        .control_reserve_bytes = directshrink::kStagingControlReserveBytes,
        .archive_offset_bytes = 3729ULL * kMiB,
        .archive_capacity_bytes = 1ULL * kGiB,
        .final_growth_owner_target_number = 5U,
    };
    data.ntfs_extension_task_count = 1U;
  }
  data.source_partition_mappings = {
      generated_mapping(
          1U, 1U, migrationcore::MigrationPartitionRole::efi_system),
      generated_mapping(
          2U, 2U,
          migrationcore::MigrationPartitionRole::microsoft_reserved),
      transferred_mapping(
          3U, 3U, migrationcore::MigrationPartitionRole::windows, true),
      transferred_mapping(
          4U, 4U, migrationcore::MigrationPartitionRole::recovery, true),
      transferred_mapping(
          5U, 5U, migrationcore::MigrationPartitionRole::data, false),
  };
  return data;
}

void test_gpt_plan_accepts_more_than_four_partitions() {
  const auto result = ytec::directshrink::make_target_plan(
      valid_gpt_plan_data());
  check(result.has_value(),
        "A reviewed GPT plan with five partitions must be accepted");
  check(result.value().tasks().size() == 5U,
        "All five GPT target tasks must remain immutable");
}

void test_operation_role_mismatch_is_rejected() {
  auto data = valid_gpt_plan_data();
  data.operation_plan.source->is_system_disk = false;
  const auto result = ytec::directshrink::make_target_plan(std::move(data));
  check(!result.has_value(),
        "OperationPlan and target-contract system roles must match exactly");
}

void test_source_bound_empty_ntfs_is_accepted() {
  const auto result = ytec::directshrink::make_target_plan(
      valid_gpt_plan_data(true));
  check(result.has_value(),
        "A selected empty NTFS data partition must use the source-bound empty task");
  check(result.value().archive_task_count() == 2U,
        "The empty NTFS task must not count as a WIM archive");
}

void test_source_bound_empty_ntfs_can_own_reviewed_surplus() {
  const auto result = ytec::directshrink::make_target_plan(
      valid_gpt_plan_data(true, true));
  check(result.has_value(),
        "A selected empty NTFS data partition may own its reviewed growth extent");
  check(result.value().surplus_target_source_table_index() == 5U,
      "The immutable surplus owner must remain the source table index");
}

void test_exact_raw_requires_original_size_type_and_no_archive() {
  auto data = valid_gpt_plan_data();
  auto& raw = data.tasks.back();
  raw.kind = ytec::directshrink::PartitionTaskKind::copy_exact_raw;
  raw.source_size_bytes = 1ULL * kGiB;
  raw.source_used_bytes = raw.source_size_bytes;
  raw.construction_size_bytes = raw.source_size_bytes;
  raw.target_size_bytes = raw.source_size_bytes;
  raw.source_partition_type = digest<std::array<std::byte, 16U>>(0x71U);
  raw.original_volume_guid_path.clear();
  raw.archive_upper_bound_bytes = 0U;
  data.archive_task_count = 2U;
  const auto accepted = ytec::directshrink::make_target_plan(data);
  check(
      accepted.has_value() &&
          accepted.value().tasks().back().kind ==
              ytec::directshrink::PartitionTaskKind::copy_exact_raw,
      "A mixed plan must retain an exact RAW task with original size and type");

  data.tasks.back().target_size_bytes -= kMiB;
  const auto resized = ytec::directshrink::make_target_plan(std::move(data));
  check(
      !resized.has_value(),
      "An exact RAW task that changes the original partition size must fail closed");
}

}  // namespace

int main() {
  try {
    test_gpt_plan_accepts_more_than_four_partitions();
    test_operation_role_mismatch_is_rejected();
    test_source_bound_empty_ntfs_is_accepted();
    test_source_bound_empty_ntfs_can_own_reviewed_surplus();
    test_exact_raw_requires_original_size_type_and_no_archive();
    std::cout << "direct shrink target contract tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#include "ytec/bootrepair/mbr2gpt_execution.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

class SequentialGuidGenerator final
    : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    ytec::clonecore::GptGuid guid;
    guid.bytes[0] = static_cast<std::byte>(next_++);
    guid.bytes[15] = static_cast<std::byte>(0xA5U);
    return ytec::clonecore::Result<
        ytec::clonecore::GptGuid>::success(guid);
  }

 private:
  unsigned int next_{1U};
};

ytec::bootrepair::Mbr2GptRebuildRequest
request_with_trailing_recovery() {
  using enum ytec::bootrepair::Mbr2GptSourceRole;
  using ytec::bootrepair::Mbr2GptSourcePartition;

  std::uint64_t cursor = 1ULL * kMiB;
  const auto partition =
      [&cursor](
          const std::uint32_t number,
          const std::uint64_t size,
          const ytec::bootrepair::Mbr2GptSourceRole role,
          const ytec::bootrepair::Mbr2GptFileSystem file_system,
          const bool active = false) {
        const Mbr2GptSourcePartition value{
            .number = number,
            .offset_bytes = cursor,
            .size_bytes = size,
            .role = role,
            .file_system = file_system,
            .primary = true,
            .active = active,
        };
        cursor += size;
        return value;
      };

  ytec::bootrepair::Mbr2GptRebuildRequest request{
      .source_disk_size_bytes = 64ULL * kGiB,
      .target_disk_size_bytes = 96ULL * kGiB,
      .logical_sector_size = 512U,
      .winre_state =
          ytec::bootrepair::WinReSourceState::
              registered_partition,
      .registered_winre_partition_number = 4U,
      .winre_image_size_bytes = 600ULL * kMiB,
      .firmware_supports_uefi = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .require_recovery_tools = true,
  };
  request.source_partitions = {
      partition(
          1U,
          500ULL * kMiB,
          system_reserved,
          ytec::bootrepair::Mbr2GptFileSystem::ntfs,
          true),
      partition(
          2U,
          50ULL * kGiB,
          windows,
          ytec::bootrepair::Mbr2GptFileSystem::ntfs),
      partition(
          3U,
          4ULL * kGiB,
          data,
          ytec::bootrepair::Mbr2GptFileSystem::ntfs),
      partition(
          4U,
          1536ULL * kMiB,
          recovery,
          ytec::bootrepair::Mbr2GptFileSystem::ntfs),
  };
  return request;
}

struct PreparedFixture final {
  ytec::bootrepair::Mbr2GptRebuildPlan layout;
  ytec::clonecore::GptWritePlan metadata;
};

PreparedFixture make_fixture() {
  auto request = request_with_trailing_recovery();
  auto layout =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(
          request);
  check(static_cast<bool>(layout), "Layout fixture should succeed");
  SequentialGuidGenerator guids;
  auto metadata =
      ytec::bootrepair::make_mbr2gpt_gpt_metadata_plan(
          layout.value(),
          request.target_disk_size_bytes,
          request.logical_sector_size,
          guids);
  check(static_cast<bool>(metadata),
        "Metadata fixture should succeed");
  return {
      .layout = layout.take_value(),
      .metadata = metadata.take_value(),
  };
}

void test_execution_contract_is_ordered_and_write_free() {
  const auto fixture = make_fixture();
  const auto result =
      ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          fixture.layout, fixture.metadata);
  check(static_cast<bool>(result),
        "Execution preparation should succeed");
  const auto& preparation = result.value();
  check(preparation.layout_and_metadata_cross_checked,
        "Layout and metadata should be cross-checked");
  check(!preparation.source_writes_permitted,
        "Source writes must never be permitted");
  check(!preparation.execution_adapter_connected,
        "Preparation must not connect an executor");
  check(!preparation.physical_write_started,
        "Preparation must not start physical writes");
  check(preparation.first_target_mutation_index == 6U,
        "Six read-only gates must precede the first mutation");
  check(
      preparation.steps[preparation.first_target_mutation_index].kind ==
          ytec::bootrepair::Mbr2GptTargetBuildStepKind::
              write_protective_mbr_and_gpt,
      "GPT metadata must be the first target mutation");
  check(
      std::all_of(
          preparation.steps.begin(),
          preparation.steps.end(),
          [](const auto& step) {
            return step.source_remains_read_only;
          }),
      "Every step must keep the source read-only");
  for (std::size_t index = 0U;
       index < preparation.steps.size();
       ++index) {
    check(preparation.steps[index].sequence == index + 1U,
          "Step sequence must be continuous");
  }

  const auto recovery_copy = std::find_if(
      preparation.steps.begin(),
      preparation.steps.end(),
      [](const auto& step) {
        return step.kind ==
                   ytec::bootrepair::Mbr2GptTargetBuildStepKind::
                       copy_source_partition_contents &&
               step.source_partition_number == 4U &&
               step.target_partition_number == 4U;
      });
  check(recovery_copy != preparation.steps.end(),
        "Trailing source recovery should be copied into target slot 4");
  const auto data_copy = std::find_if(
      preparation.steps.begin(),
      preparation.steps.end(),
      [](const auto& step) {
        return step.kind ==
                   ytec::bootrepair::Mbr2GptTargetBuildStepKind::
                       copy_source_partition_contents &&
               step.source_partition_number == 3U &&
               step.target_partition_number == 5U;
      });
  check(data_copy != preparation.steps.end(),
        "Source data should move after the target recovery slot");
  check(recovery_copy < data_copy,
        "Recovery copy must occur before data copy");
}

void test_missing_recovery_is_created_and_staged() {
  auto request = request_with_trailing_recovery();
  request.source_partitions.pop_back();
  request.winre_state =
      ytec::bootrepair::WinReSourceState::
          image_available_in_windows;
  request.registered_winre_partition_number = 0U;
  auto layout =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(
          request);
  check(static_cast<bool>(layout),
        "Verified Windows WinRE image should allow recovery creation");
  SequentialGuidGenerator guids;
  auto metadata =
      ytec::bootrepair::make_mbr2gpt_gpt_metadata_plan(
          layout.value(),
          request.target_disk_size_bytes,
          request.logical_sector_size,
          guids);
  check(static_cast<bool>(metadata),
        "Metadata for staged WinRE should succeed");
  const auto result =
      ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          layout.value(), metadata.value());
  check(static_cast<bool>(result),
        "Staged WinRE execution preparation should succeed");
  const auto has_recovery_format = std::any_of(
      result.value().steps.begin(),
      result.value().steps.end(),
      [](const auto& step) {
        return step.kind ==
               ytec::bootrepair::Mbr2GptTargetBuildStepKind::
                   create_recovery_ntfs;
      });
  const auto has_winre_stage = std::any_of(
      result.value().steps.begin(),
      result.value().steps.end(),
      [](const auto& step) {
        return step.kind ==
               ytec::bootrepair::Mbr2GptTargetBuildStepKind::
                   stage_winre_image;
      });
  check(has_recovery_format && has_winre_stage,
        "Missing recovery must be created and populated explicitly");
}

void test_tampered_partition_metadata_is_rejected() {
  auto fixture = make_fixture();
  fixture.metadata.target_disk.partitions[3].attributes = 0U;
  check(
      !ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          fixture.layout, fixture.metadata),
      "Missing recovery GPT attributes must fail");

  fixture = make_fixture();
  fixture.metadata.target_disk.partitions[0].type_guid =
      ytec::clonecore::gpt_type_basic_data();
  check(
      !ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          fixture.layout, fixture.metadata),
      "Wrong ESP type GUID must fail");

  fixture = make_fixture();
  fixture.metadata.target_disk.partitions[1].unique_guid =
      fixture.metadata.target_disk.partitions[0].unique_guid;
  check(
      !ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          fixture.layout, fixture.metadata),
      "Duplicate unique partition GUID must fail");
}

void test_tampered_write_order_is_rejected() {
  auto fixture = make_fixture();
  std::swap(
      fixture.metadata.writes[0],
      fixture.metadata.writes[1]);
  check(
      !ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          fixture.layout, fixture.metadata),
      "GPT commit order tampering must fail");
}

void test_source_mutation_permission_is_rejected() {
  auto fixture = make_fixture();
  fixture.layout.source_disk_remains_unchanged = false;
  check(
      !ytec::bootrepair::prepare_mbr2gpt_target_build_execution(
          fixture.layout, fixture.metadata),
      "A plan allowing source changes must fail");
}

}  // namespace

int main() {
  try {
    test_execution_contract_is_ordered_and_write_free();
    test_missing_recovery_is_created_and_staged();
    test_tampered_partition_metadata_is_rejected();
    test_tampered_write_order_is_rejected();
    test_source_mutation_permission_is_rejected();
    std::cout << "mbr2gpt execution preparation tests passed\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << failure.message << '\n';
    return 1;
  }
}

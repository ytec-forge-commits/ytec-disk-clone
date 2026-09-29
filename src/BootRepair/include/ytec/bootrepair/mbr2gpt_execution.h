#pragma once

#include "ytec/bootrepair/mbr2gpt_layout.h"
#include "ytec/clonecore/gpt.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

enum class Mbr2GptTargetBuildStepKind : std::uint8_t {
  verify_source_identity_read_only,
  run_official_mbr2gpt_validate,
  reidentify_source_after_validate,
  verify_target_identity_and_empty,
  verify_two_step_target_confirmation,
  verify_microsoft_tools,
  write_protective_mbr_and_gpt,
  create_esp_fat32,
  copy_source_partition_contents,
  extend_windows_file_system,
  create_recovery_ntfs,
  stage_winre_image,
  run_bcdboot_uefi,
  register_winre,
  verify_gpt_readback,
  verify_partition_contents,
  verify_uefi_boot_files,
  verify_winre_registration,
  require_cold_uefi_boot_test,
};

struct Mbr2GptTargetBuildStep final {
  std::uint32_t sequence{};
  Mbr2GptTargetBuildStepKind kind{
      Mbr2GptTargetBuildStepKind::
          verify_source_identity_read_only};
  std::uint32_t source_partition_number{};
  std::uint32_t target_partition_number{};
  bool mutates_target{};
  bool source_remains_read_only{true};
};

struct Mbr2GptExecutionPreparation final {
  std::vector<Mbr2GptTargetBuildStep> steps;
  std::size_t first_target_mutation_index{};
  bool layout_and_metadata_cross_checked{};
  bool source_writes_permitted{};
  bool official_mbr2gpt_validate_required{true};
  bool bcdboot_signature_check_required{true};
  bool winre_registration_required{};
  bool final_cold_uefi_boot_required{true};
  bool execution_adapter_connected{};
  bool physical_write_started{};
};

// Cross-checks the semantic layout against the exact in-memory GPT bytes and
// emits an ordered execution contract. No adapter is invoked and no device is
// opened. The result always keeps execution_adapter_connected=false and
// physical_write_started=false; a future target-only executor must perform
// each read-only gate again immediately before the first mutation.
[[nodiscard]] clonecore::Result<Mbr2GptExecutionPreparation>
prepare_mbr2gpt_target_build_execution(
    const Mbr2GptRebuildPlan& layout,
    const clonecore::GptWritePlan& metadata);

[[nodiscard]] std::wstring_view
mbr2gpt_target_build_step_name(
    Mbr2GptTargetBuildStepKind kind) noexcept;

}  // namespace ytec::bootrepair

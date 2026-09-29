#pragma once

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/shrink_image_manifest.h"
#include "ytec/migrationcore/shrink_layout.h"

#include <cstdint>
#include <span>

namespace ytec::migrationengine {

struct ShrinkTargetLayout final {
  migrationcore::ShrinkMigrationPlan migration;
  clonecore::GptWritePlan gpt;
  clonecore::MbrWritePlan mbr;
  bool is_gpt{};
};

// Pure target-only planning. The source manifest is never modified. GPT uses
// fresh disk/partition GUIDs; MBR uses a fresh non-conflicting signature and
// the manifest-bound source bootstrap code.
[[nodiscard]] clonecore::Result<ShrinkTargetLayout>
build_shrink_target_layout(
    const imageformat::ShrinkImageManifest& source_manifest,
    std::uint64_t target_size_bytes,
    std::uint32_t target_logical_sector_size,
    clonecore::IGuidGenerator& guid_generator,
    clonecore::IMbrSignatureGenerator& signature_generator,
    std::span<const std::uint32_t> disallowed_mbr_signatures = {});

}  // namespace ytec::migrationengine

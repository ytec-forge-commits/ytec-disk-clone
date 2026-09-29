#pragma once

#include "ytec/imageformat/sha256.h"

#include <cstdint>

namespace ytec::imageformat {

// Direct operations and the transitional legacy manifest share these neutral
// value types. They do not imply a persisted reservation or job file.
enum class TransferMode : std::uint8_t {
  exact,
  shrink,
};

struct RestoreImageIdentity final {
  std::uint64_t length_bytes{};
  Sha256Digest global_hash{};
};

}  // namespace ytec::imageformat

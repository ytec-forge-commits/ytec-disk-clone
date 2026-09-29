#pragma once

#include "ytec/operationcore/operation.h"

#include <span>

namespace ytec::operationcore::detail {

[[nodiscard]] clonecore::Result<Sha256Digest> sha256(
    std::span<const std::byte> bytes);

[[nodiscard]] bool digest_is_zero(const Sha256Digest& digest) noexcept;

[[nodiscard]] bool digest_equal(
    const Sha256Digest& left,
    const Sha256Digest& right) noexcept;

}  // namespace ytec::operationcore::detail

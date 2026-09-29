#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ytec::imageformat {

using Sha256Digest = std::array<std::byte, 32>;

[[nodiscard]] clonecore::Result<Sha256Digest> sha256(
    std::span<const std::byte> bytes);

[[nodiscard]] clonecore::Result<Sha256Digest> sha256_zeroes(
    std::uint64_t length);

using Sha256ReadCallback = std::function<
    clonecore::Result<std::vector<std::byte>>(
        std::uint64_t offset,
        std::size_t length)>;

// 大きな出力を全てメモリへ載せず、固定上限の読戻しでSHA-256を計算する。
// Callbackは要求された長さを正確に返さなければ失敗する。
[[nodiscard]] clonecore::Result<Sha256Digest> sha256_from_reader(
    std::uint64_t length,
    std::size_t maximum_block_bytes,
    const Sha256ReadCallback& reader);

}  // namespace ytec::imageformat

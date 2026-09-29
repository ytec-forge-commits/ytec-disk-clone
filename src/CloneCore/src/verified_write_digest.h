#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ytec::clonecore::detail {

using VerifiedWriteDigest = std::array<std::byte, 32>;

// Accumulates evidence only after a target write has been read back and
// compared successfully. The serialized record format is deliberately fixed:
// domain || offset-le64 || length-le64 || read-back bytes.
class VerifiedWriteDigestBuilder final {
 public:
  [[nodiscard]] static Result<VerifiedWriteDigestBuilder> create();

  ~VerifiedWriteDigestBuilder();
  VerifiedWriteDigestBuilder(VerifiedWriteDigestBuilder&&) noexcept;
  VerifiedWriteDigestBuilder& operator=(VerifiedWriteDigestBuilder&&) noexcept;

  VerifiedWriteDigestBuilder(const VerifiedWriteDigestBuilder&) = delete;
  VerifiedWriteDigestBuilder& operator=(const VerifiedWriteDigestBuilder&) =
      delete;

  [[nodiscard]] Status append_verified_write(
      std::uint64_t offset,
      std::span<const std::byte> read_back_bytes);

  [[nodiscard]] Result<VerifiedWriteDigest> finish();

 private:
  struct State;

  explicit VerifiedWriteDigestBuilder(std::unique_ptr<State> state) noexcept;

  std::unique_ptr<State> state_;
};

}  // namespace ytec::clonecore::detail

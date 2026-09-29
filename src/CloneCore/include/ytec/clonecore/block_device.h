#pragma once

#include "ytec/clonecore/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ytec::clonecore {

struct ByteRange final {
  std::uint64_t offset{};
  std::uint64_t length{};
};

class ISourceDiskReader {
 public:
  virtual ~ISourceDiskReader() = default;

  [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t logical_sector_size() const noexcept = 0;
  [[nodiscard]] virtual Result<std::vector<std::byte>> read(
      std::uint64_t offset,
      std::size_t length) const = 0;
};

class ITargetDiskWriter {
 public:
  virtual ~ITargetDiskWriter() = default;

  [[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t logical_sector_size() const noexcept = 0;
  [[nodiscard]] virtual Status write_target(
      std::uint64_t offset,
      std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual Result<std::vector<std::byte>> read_back(
      std::uint64_t offset,
      std::size_t length) const = 0;
  [[nodiscard]] virtual Status flush_target() = 0;
};

}  // namespace ytec::clonecore

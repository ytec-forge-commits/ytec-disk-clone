#include "ytec/clonecore/crc32.h"

namespace ytec::clonecore {

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept {
  std::uint32_t value = 0xFFFFFFFFU;
  for (const std::byte byte : bytes) {
    value ^= std::to_integer<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(-(static_cast<std::int32_t>(value & 1U)));
      value = (value >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return value ^ 0xFFFFFFFFU;
}

}  // namespace ytec::clonecore

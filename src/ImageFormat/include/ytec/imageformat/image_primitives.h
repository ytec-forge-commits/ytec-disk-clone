#pragma once

#include <cstdint>

namespace ytec::imageformat {

inline constexpr std::uint32_t kImageChunkSize16MiB =
    16U * 1024U * 1024U;
inline constexpr std::uint32_t kImageChunkSize32MiB =
    32U * 1024U * 1024U;

[[nodiscard]] constexpr bool is_supported_sector_size_pair(
    const std::uint32_t logical,
    const std::uint32_t physical) noexcept {
  const bool physical_is_power_of_two =
      physical != 0U && (physical & (physical - 1U)) == 0U;
  return (logical == 512U || logical == 4096U) &&
         physical_is_power_of_two && physical >= logical &&
         physical <= 65'536U && physical % logical == 0U;
}

enum class ImageCompression : std::uint16_t {
  none = 0,
  zstandard = 1,
};

struct ImageSection final {
  std::uint64_t offset{};
  std::uint64_t length{};
};

}  // namespace ytec::imageformat

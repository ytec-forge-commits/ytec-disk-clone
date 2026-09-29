#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ytec::winpeapp {

struct ProductIoPolicy final {
  std::size_t transfer_chunk_bytes{};
  std::size_t maximum_bytes_between_cancellation_checks{};
};

struct ProductIoEstimate final {
  std::uint64_t logical_chunk_count{};
  std::uint64_t source_read_count{};
  std::uint64_t target_write_count{};
  std::uint64_t target_read_back_count{};
};

// Product writes remain restricted to 512-byte logical sectors until the
// physical 4Kn acceptance matrix is complete. The 16 MiB transfer size is the
// existing validated upper bound shared by GPT/MBR clone and dcimg restore.
[[nodiscard]] std::optional<ProductIoPolicy> select_product_io_policy(
    std::uint32_t logical_sector_size) noexcept;

// Deterministic request-count estimate. It is used for regression tests and
// reporting only; wall-clock throughput must be measured in the dedicated VM.
[[nodiscard]] ProductIoEstimate estimate_product_io_requests(
    std::uint64_t logical_data_bytes,
    const ProductIoPolicy& policy) noexcept;

}  // namespace ytec::winpeapp

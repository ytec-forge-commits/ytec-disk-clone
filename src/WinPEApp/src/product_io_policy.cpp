#include "ytec/winpeapp/product_io_policy.h"

namespace ytec::winpeapp {
namespace {

constexpr std::size_t kProductTransferChunkBytes =
    16U * 1024U * 1024U;

}  // namespace

std::optional<ProductIoPolicy> select_product_io_policy(
    const std::uint32_t logical_sector_size) noexcept {
  if (logical_sector_size != 512U) {
    return std::nullopt;
  }
  return ProductIoPolicy{
      .transfer_chunk_bytes = kProductTransferChunkBytes,
      .maximum_bytes_between_cancellation_checks =
          kProductTransferChunkBytes,
  };
}

ProductIoEstimate estimate_product_io_requests(
    const std::uint64_t logical_data_bytes,
    const ProductIoPolicy& policy) noexcept {
  if (logical_data_bytes == 0U || policy.transfer_chunk_bytes == 0U) {
    return {};
  }
  const std::uint64_t chunk_bytes = policy.transfer_chunk_bytes;
  const std::uint64_t chunks = logical_data_bytes / chunk_bytes +
      (logical_data_bytes % chunk_bytes == 0U ? 0U : 1U);
  return ProductIoEstimate{
      .logical_chunk_count = chunks,
      .source_read_count = chunks,
      .target_write_count = chunks,
      .target_read_back_count = chunks,
  };
}

}  // namespace ytec::winpeapp

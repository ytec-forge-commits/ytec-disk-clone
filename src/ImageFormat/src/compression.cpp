#include "ytec/imageformat/compression.h"

#include <Windows.h>
#include <zstd.h>

#include <array>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace ytec::imageformat {
namespace {

static_assert(
    ZSTD_VERSION_NUMBER == 10507,
    "image chunk v1 must be built with reviewed Zstandard v1.5.7");

clonecore::Error compression_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Error invalid_profile(
    std::wstring operation,
    std::wstring message) {
  return compression_error(
      clonecore::ErrorCode::invalid_data,
      ERROR_INVALID_DATA,
      std::move(operation),
      std::move(message));
}

}  // namespace

clonecore::Result<std::vector<std::byte>>
compress_zstandard_image_chunk_v1(
    const std::span<const std::byte> uncompressed) {
  if (uncompressed.empty() ||
      uncompressed.size() > kImageChunkSize32MiB ||
      ZSTD_versionNumber() != ZSTD_VERSION_NUMBER) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        compression_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"イメージチャンク Zstandard圧縮条件",
            L"入力長またはZstandard版が固定イメージプロファイル外です"));
  }

  const std::size_t capacity = ZSTD_compressBound(uncompressed.size());
  if (ZSTD_isError(capacity) != 0U || capacity < uncompressed.size()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandard圧縮上限",
            L"圧縮先の安全な上限を計算できませんでした"));
  }
  std::vector<std::byte> compressed;
  try {
    compressed.resize(capacity);
  } catch (const std::bad_alloc&) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        compression_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"イメージチャンク Zstandard圧縮メモリ",
            L"圧縮用メモリを確保できませんでした"));
  }

  std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(
      ZSTD_createCCtx(), &ZSTD_freeCCtx);
  if (!context) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        compression_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"イメージチャンク Zstandard圧縮コンテキスト",
            L"圧縮コンテキストを確保できませんでした"));
  }
  const std::array<std::pair<ZSTD_cParameter, int>, 4> parameters{
      std::pair{ZSTD_c_compressionLevel, kImageZstandardCompressionLevel},
      std::pair{ZSTD_c_contentSizeFlag, 1},
      std::pair{ZSTD_c_checksumFlag, 1},
      std::pair{ZSTD_c_dictIDFlag, 0},
  };
  for (const auto& [parameter, value] : parameters) {
    if (ZSTD_isError(
            ZSTD_CCtx_setParameter(context.get(), parameter, value)) != 0U) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          invalid_profile(
              L"イメージチャンク Zstandard圧縮パラメーター",
              L"固定圧縮プロファイルを設定できませんでした"));
    }
  }
  const std::size_t result = ZSTD_compress2(
      context.get(),
      compressed.data(),
      compressed.size(),
      uncompressed.data(),
      uncompressed.size());
  if (ZSTD_isError(result) != 0U || result == 0U || result > capacity) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandard圧縮",
            L"Zstandard圧縮に失敗しました"));
  }
  compressed.resize(result);
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(compressed));
}

clonecore::Result<std::vector<std::byte>>
decompress_zstandard_image_chunk_v1(
    const std::span<const std::byte> stored,
    const std::size_t expected_uncompressed_length) {
  if (stored.empty() || expected_uncompressed_length == 0U ||
      expected_uncompressed_length > kImageChunkSize32MiB ||
      stored.size() >= expected_uncompressed_length ||
      ZSTD_versionNumber() != ZSTD_VERSION_NUMBER) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandard展開条件",
            L"保存長、展開長、またはZstandard版がプロファイル外です"));
  }

  const std::size_t frame_size =
      ZSTD_findFrameCompressedSize(stored.data(), stored.size());
  if (ZSTD_isError(frame_size) != 0U || frame_size != stored.size()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandardフレーム境界",
            L"単一の完全なZstandardフレームではありません"));
  }
  // Standard Zstandard frames place the content-checksum flag at bit 2 of
  // the frame-header descriptor immediately after the four-byte magic.
  if (stored.size() < 5U ||
      (std::to_integer<unsigned int>(stored[4]) & 0x04U) == 0U) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandardチェックサム",
            L"必須のフレームチェックサムがありません"));
  }
  const unsigned long long content_size =
      ZSTD_getFrameContentSize(stored.data(), stored.size());
  if (content_size == ZSTD_CONTENTSIZE_ERROR ||
      content_size == ZSTD_CONTENTSIZE_UNKNOWN ||
      content_size != expected_uncompressed_length ||
      ZSTD_getDictID_fromFrame(stored.data(), stored.size()) != 0U) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandardフレーム情報",
            L"展開長が不一致、未知、または辞書付きフレームです"));
  }

  std::vector<std::byte> uncompressed;
  try {
    uncompressed.resize(expected_uncompressed_length);
  } catch (const std::bad_alloc&) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        compression_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"イメージチャンク Zstandard展開メモリ",
            L"展開用メモリを確保できませんでした"));
  }
  const std::size_t result = ZSTD_decompress(
      uncompressed.data(),
      uncompressed.size(),
      stored.data(),
      stored.size());
  if (ZSTD_isError(result) != 0U || result != uncompressed.size()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        invalid_profile(
            L"イメージチャンク Zstandard展開",
            L"Zstandard展開結果が宣言長と一致しません"));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(uncompressed));
}

clonecore::Result<std::vector<std::byte>>
compress_zstandard_dcimg_v1(
    const std::span<const std::byte> uncompressed) {
  return compress_zstandard_image_chunk_v1(uncompressed);
}

clonecore::Result<std::vector<std::byte>>
decompress_zstandard_dcimg_v1(
    const std::span<const std::byte> stored,
    const std::size_t expected_uncompressed_length) {
  return decompress_zstandard_image_chunk_v1(
      stored, expected_uncompressed_length);
}

}  // namespace ytec::imageformat

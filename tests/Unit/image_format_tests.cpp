#include "ytec/imageformat/compression.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/sha256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
T read_little(const std::vector<std::byte>& bytes, const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::string to_hex(const ytec::imageformat::Sha256Digest& digest) {
  constexpr std::array<char, 16> digits{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string output;
  output.reserve(digest.size() * 2);
  for (const std::byte value : digest) {
    const auto number = std::to_integer<unsigned int>(value);
    output.push_back(digits[(number >> 4U) & 0x0FU]);
    output.push_back(digits[number & 0x0FU]);
  }
  return output;
}

ytec::imageformat::DcimgBuildRequest sample_request() {
  using ytec::imageformat::DcimgBuildChunk;
  using ytec::imageformat::DcimgBuildRequest;

  DcimgBuildRequest request;
  request.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.chunk_size = ytec::imageformat::kDcimgChunkSize16MiB;
  request.manifest = {
      std::byte{'M'}, std::byte{'A'}, std::byte{'N'}, std::byte{'1'}};
  request.partition_table_snapshot.assign(512, std::byte{0});

  DcimgBuildChunk first;
  first.logical_offset = 0;
  first.logical_length = 4096;
  first.data.assign(4096, std::byte{0x11});
  request.chunks.push_back(std::move(first));

  DcimgBuildChunk zero;
  zero.logical_offset = 16ULL * 1024ULL * 1024ULL;
  zero.logical_length = 8192;
  zero.zero_filled = true;
  request.chunks.push_back(std::move(zero));

  DcimgBuildChunk last;
  last.logical_offset = 32ULL * 1024ULL * 1024ULL;
  last.logical_length = 4096;
  last.data.assign(4096, std::byte{0x22});
  request.chunks.push_back(std::move(last));
  return request;
}

std::vector<std::byte> sample_image() {
  const auto result =
      ytec::imageformat::build_uncompressed_dcimg_v1(sample_request());
  check(result.has_value(), "A valid synthetic image should build");
  return result.value();
}

void refresh_global_hash(std::vector<std::byte>& image) {
  const std::uint64_t footer_offset = read_little<std::uint64_t>(image, 136);
  const auto digest = ytec::imageformat::sha256(std::span<const std::byte>(
      image.data(), static_cast<std::size_t>(footer_offset)));
  check(digest.has_value(), "Global hash refresh should succeed");
  std::copy(
      digest.value().begin(),
      digest.value().end(),
      image.begin() + static_cast<std::ptrdiff_t>(footer_offset + 8));
}

void test_sha256_known_vector() {
  const std::array<std::byte, 3> input{
      std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  const auto result = ytec::imageformat::sha256(input);
  check(result.has_value(), "Windows CNG SHA-256 should succeed");
  check(
      to_hex(result.value()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "SHA-256 must match the standard abc vector");
}

void test_round_trip_uncompressed_and_zero_chunks() {
  const auto image = sample_image();
  const auto result = ytec::imageformat::inspect_dcimg_v1(image);
  check(result.has_value(), "A freshly built v1 image should verify");
  check(result.value().chunks.size() == 3, "All chunk records should parse");
  check(
      result.value().chunks[1].flags ==
          ytec::imageformat::DcimgChunkFlags::zero_filled,
      "The zero-filled chunk flag should survive");
  check(
      result.value().manifest_hash_verified &&
          result.value().chunk_hashes_verified &&
          result.value().global_hash_verified,
      "All implemented integrity layers should verify");
}

void test_reader_inspection_matches_memory_inspection() {
  const auto image = sample_image();
  std::size_t maximum_request{};
  std::size_t progress_calls{};
  ytec::imageformat::DcimgReadInspectionProgress final_progress;
  const auto result =
      ytec::imageformat::inspect_dcimg_v1_from_reader(
          image.size(),
          1024,
          [&image, &maximum_request](
              const std::uint64_t offset,
              const std::size_t length) {
            maximum_request = (std::max)(maximum_request, length);
            if (offset > image.size() ||
                length > image.size() - offset) {
              return ytec::clonecore::Result<
                  std::vector<std::byte>>::failure(
                  ytec::clonecore::Error{
                      .code =
                          ytec::clonecore::ErrorCode::invalid_data,
                      .native_code = ERROR_HANDLE_EOF,
                      .operation = L"mock dcimg reader",
                      .message = L"range outside image",
                  });
            }
            const auto begin =
                image.begin() + static_cast<std::ptrdiff_t>(offset);
            return ytec::clonecore::Result<
                std::vector<std::byte>>::success(
                std::vector<std::byte>(
                    begin,
                    begin + static_cast<std::ptrdiff_t>(length)));
          },
          ytec::imageformat::DcimgReadInspectionCallbacks{
              .progress =
                  [&progress_calls, &final_progress](
                      const ytec::imageformat::
                          DcimgReadInspectionProgress& progress) {
                    ++progress_calls;
                    final_progress = progress;
                  },
          });
  check(result.has_value(), "Reader inspection should verify a valid image");
  check(
      result.value().container.chunks.size() == 3,
      "Reader inspection should preserve all chunks");
  check(
      result.value().manifest == sample_request().manifest,
      "Reader inspection should return verified manifest bytes");
  check(
      maximum_request <= 1024U,
      "Small synthetic metadata should use bounded reads");
  check(
      progress_calls > 1 &&
          final_progress.verified_bytes ==
              final_progress.total_verify_bytes,
      "Reader inspection progress should finish at the exact work total");
}

void test_reader_inspection_rejects_change_during_verification() {
  const auto image = sample_image();
  std::size_t reads_at_zero{};
  const auto result =
      ytec::imageformat::inspect_dcimg_v1_from_reader(
          image.size(),
          1024,
          [&image, &reads_at_zero](
              const std::uint64_t offset,
              const std::size_t length) {
            const auto begin =
                image.begin() + static_cast<std::ptrdiff_t>(offset);
            std::vector<std::byte> bytes(
                begin,
                begin + static_cast<std::ptrdiff_t>(length));
            if (offset == 0) {
              ++reads_at_zero;
              if (reads_at_zero >= 3 && !bytes.empty()) {
                bytes[0] ^= std::byte{0x01};
              }
            }
            return ytec::clonecore::Result<
                std::vector<std::byte>>::success(std::move(bytes));
          });
  check(
      !result.has_value(),
      "Header changes during verification must fail closed");
  check(
      result.error().operation == L"dcimg検証中の変更",
      "A changed file should report the stability gate");
}

void test_truncated_image_is_rejected() {
  auto image = sample_image();
  image.pop_back();
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "A truncated footer must be rejected");
}

void test_unknown_version_is_rejected() {
  auto image = sample_image();
  write_little<std::uint16_t>(image, 8, 2);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "An unknown major version must be rejected before restore");
}

void test_section_overflow_is_rejected() {
  auto image = sample_image();
  write_little<std::uint64_t>(image, 56, UINT64_MAX - 8);
  write_little<std::uint64_t>(image, 64, 32);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "An overflowing section range must be rejected");
}

void test_overlapping_logical_chunks_are_rejected() {
  auto image = sample_image();
  const std::uint64_t index_offset = read_little<std::uint64_t>(image, 88);
  write_little<std::uint64_t>(
      image,
      static_cast<std::size_t>(
          index_offset + ytec::imageformat::kDcimgChunkRecordSize),
      0);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "Duplicate or overlapping logical offsets must be rejected");
}

void test_duplicate_stored_offsets_are_rejected() {
  auto image = sample_image();
  const std::uint64_t index_offset = read_little<std::uint64_t>(image, 88);
  const std::uint64_t first_stored =
      read_little<std::uint64_t>(image, static_cast<std::size_t>(index_offset + 16));
  write_little<std::uint64_t>(
      image,
      static_cast<std::size_t>(
          index_offset + 2ULL * ytec::imageformat::kDcimgChunkRecordSize + 16),
      first_stored);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "Duplicate stored offsets must be rejected");
}

void test_corrupted_data_hash_is_rejected() {
  auto image = sample_image();
  const std::uint64_t data_offset = read_little<std::uint64_t>(image, 104);
  image[static_cast<std::size_t>(data_offset)] ^= std::byte{0x01};
  refresh_global_hash(image);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "A chunk hash mismatch must reject the image");
}

void test_hash_table_disagreement_is_rejected() {
  auto image = sample_image();
  const std::uint64_t hash_offset = read_little<std::uint64_t>(image, 120);
  image[static_cast<std::size_t>(hash_offset)] ^= std::byte{0x01};
  refresh_global_hash(image);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "The independent hash table must match the chunk index");
}

void test_zero_chunk_hash_is_verified() {
  auto image = sample_image();
  const std::uint64_t index_offset = read_little<std::uint64_t>(image, 88);
  const std::uint64_t hash_offset = read_little<std::uint64_t>(image, 120);
  const std::size_t record_hash = static_cast<std::size_t>(
      index_offset + ytec::imageformat::kDcimgChunkRecordSize + 40);
  const std::size_t table_hash = static_cast<std::size_t>(hash_offset + 32);
  image[record_hash] ^= std::byte{0x01};
  image[table_hash] ^= std::byte{0x01};
  refresh_global_hash(image);
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "A zero-filled chunk must be hashed without stored data");
}

void test_reserved_bytes_are_rejected() {
  auto image = sample_image();
  image[184] = std::byte{0x01};
  check(
      !ytec::imageformat::inspect_dcimg_v1(image).has_value(),
      "Unknown header extensions must fail closed in v1");
}

void test_invalid_build_requests_are_rejected() {
  auto request = sample_request();
  request.logical_sector_size = 0;
  check(
      !ytec::imageformat::build_uncompressed_dcimg_v1(request).has_value(),
      "A zero sector size must fail without division by zero");

  request = sample_request();
  request.chunks[1].logical_offset = 0;
  check(
      !ytec::imageformat::build_uncompressed_dcimg_v1(request).has_value(),
      "The builder must reject overlapping chunks");

  request = sample_request();
  request.chunks[1].data.push_back(std::byte{0});
  check(
      !ytec::imageformat::build_uncompressed_dcimg_v1(request).has_value(),
      "A zero chunk must not carry stored data");
}

void test_zstandard_profile_round_trip_and_corruption_rejection() {
  std::vector<std::byte> source(1024U * 1024U, std::byte{0x2A});
  const auto compressed =
      ytec::imageformat::compress_zstandard_dcimg_v1(source);
  check(compressed.has_value(), "Zstandard profile compression should succeed");
  check(compressed.value().size() < source.size(),
        "Compressible input should become smaller than the source");

  const auto restored = ytec::imageformat::decompress_zstandard_dcimg_v1(
      compressed.value(), source.size());
  check(restored.has_value() && restored.value() == source,
        "Exact single-frame Zstandard data should round-trip");

  auto corrupted = compressed.value();
  corrupted[corrupted.size() / 2U] ^= std::byte{0x40};
  check(
      !ytec::imageformat::decompress_zstandard_dcimg_v1(
           corrupted, source.size()).has_value(),
      "Corrupted compressed data must be rejected");

  auto trailing = compressed.value();
  trailing.push_back(std::byte{0});
  check(
      !ytec::imageformat::decompress_zstandard_dcimg_v1(
           trailing, source.size()).has_value(),
      "Trailing bytes or concatenated content must be rejected");
  auto checksumless = compressed.value();
  check(checksumless.size() > 8U,
        "The compressed fixture should contain a checksum trailer");
  checksumless[4] &= std::byte{0xFBU};
  checksumless.resize(checksumless.size() - 4U);
  check(
      !ytec::imageformat::decompress_zstandard_dcimg_v1(
           checksumless, source.size()).has_value(),
      "A frame without the mandatory content checksum must be rejected");
  check(
      !ytec::imageformat::decompress_zstandard_dcimg_v1(
           compressed.value(), source.size() - 512U).has_value(),
      "A declared expansion length mismatch must be rejected");
}

void test_zstandard_dcimg_round_trip_and_chunk_corruption_rejection() {
  auto request = sample_request();
  request.compression = ytec::imageformat::DcimgCompression::zstandard;
  const auto built = ytec::imageformat::build_dcimg_v1(request);
  check(built.has_value(), "Zstandard dcimg construction should succeed");

  const auto inspected = ytec::imageformat::inspect_dcimg_v1(built.value());
  check(inspected.has_value(), "Compressed dcimg should fully verify");
  check(
      inspected.value().header.compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          inspected.value().header.compression_version == 1U,
      "Header must identify the reviewed Zstandard profile");
  check(
      inspected.value().chunks[0].compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          inspected.value().chunks[0].stored_length <
              inspected.value().chunks[0].uncompressed_length,
      "Compressible data chunks should use a strictly smaller frame");
  check(
      inspected.value().chunks[1].flags ==
              ytec::imageformat::DcimgChunkFlags::zero_filled &&
          inspected.value().chunks[1].compression ==
              ytec::imageformat::DcimgCompression::none,
      "Zero chunks must remain implicit and uncompressed");

  const auto reader_inspection =
      ytec::imageformat::inspect_dcimg_v1_from_reader(
          built.value().size(),
          1024U * 1024U,
          [&image = built.value()](
              const std::uint64_t offset,
              const std::size_t length) {
            if (offset > image.size() || length > image.size() - offset) {
              return ytec::clonecore::Result<std::vector<std::byte>>::failure(
                  ytec::clonecore::Error{
                      .code = ytec::clonecore::ErrorCode::io_failed,
                      .native_code = ERROR_HANDLE_EOF,
                      .operation = L"圧縮dcimgモック読取り",
                      .message = L"範囲外読取り",
                  });
            }
            return ytec::clonecore::Result<std::vector<std::byte>>::success(
                std::vector<std::byte>(
                    image.begin() + static_cast<std::ptrdiff_t>(offset),
                    image.begin() +
                        static_cast<std::ptrdiff_t>(offset + length)));
          });
  check(reader_inspection.has_value(),
        "Bounded reader inspection must verify compressed chunks");

  auto corrupted = built.value();
  const std::size_t corrupt_offset = static_cast<std::size_t>(
      inspected.value().chunks[0].stored_offset +
      inspected.value().chunks[0].stored_length / 2U);
  corrupted[corrupt_offset] ^= std::byte{0x20};
  refresh_global_hash(corrupted);
  check(!ytec::imageformat::inspect_dcimg_v1(corrupted).has_value(),
        "Corrupted compressed payload must fail after a valid outer hash");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"sha256_known_vector", test_sha256_known_vector},
      {"round_trip_uncompressed_and_zero_chunks",
       test_round_trip_uncompressed_and_zero_chunks},
      {"reader_inspection_matches_memory_inspection",
       test_reader_inspection_matches_memory_inspection},
      {"reader_inspection_rejects_change_during_verification",
       test_reader_inspection_rejects_change_during_verification},
      {"truncated_image_is_rejected", test_truncated_image_is_rejected},
      {"unknown_version_is_rejected", test_unknown_version_is_rejected},
      {"section_overflow_is_rejected", test_section_overflow_is_rejected},
      {"overlapping_logical_chunks_are_rejected",
       test_overlapping_logical_chunks_are_rejected},
      {"duplicate_stored_offsets_are_rejected",
       test_duplicate_stored_offsets_are_rejected},
      {"corrupted_data_hash_is_rejected", test_corrupted_data_hash_is_rejected},
      {"hash_table_disagreement_is_rejected",
       test_hash_table_disagreement_is_rejected},
      {"zero_chunk_hash_is_verified", test_zero_chunk_hash_is_verified},
      {"reserved_bytes_are_rejected", test_reserved_bytes_are_rejected},
      {"invalid_build_requests_are_rejected",
       test_invalid_build_requests_are_rejected},
      {"zstandard_profile_round_trip_and_corruption_rejection",
       test_zstandard_profile_round_trip_and_corruption_rejection},
      {"zstandard_dcimg_round_trip_and_chunk_corruption_rejection",
       test_zstandard_dcimg_round_trip_and_chunk_corruption_rejection},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

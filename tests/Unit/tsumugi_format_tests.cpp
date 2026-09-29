#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"

#include <Windows.h>

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

std::string utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return "<wide conversion failed>";
  }
  std::string result(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
          static_cast<int>(value.size()), result.data(), length,
          nullptr, nullptr) != length) {
    return "<wide conversion failed>";
  }
  return result;
}

void print_error(const ytec::clonecore::Error& error) {
  std::cerr << "DETAIL " << utf8(error.operation) << ": "
            << utf8(error.message) << " native=" << error.native_code
            << '\n';
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

void refresh_global_hash(std::vector<std::byte>& image) {
  const auto footer_offset = read_little<std::uint64_t>(image, 96U);
  const auto digest = ytec::imageformat::sha256(
      std::span<const std::byte>(
          image.data(), static_cast<std::size_t>(footer_offset)));
  check(digest.has_value(), "The synthetic global hash must be calculable");
  std::copy(
      digest.value().begin(), digest.value().end(),
      image.begin() + static_cast<std::ptrdiff_t>(footer_offset + 16U));
}

void refresh_header_hash(std::vector<std::byte>& image) {
  std::fill(image.begin() + 188U, image.begin() + 220U, std::byte{0});
  const auto digest = ytec::imageformat::sha256(
      std::span<const std::byte>(
          image.data(), ytec::imageformat::kTsumugiHeaderSize));
  check(digest.has_value(), "The synthetic header hash must be calculable");
  std::copy(digest.value().begin(), digest.value().end(), image.begin() + 188U);
}

ytec::imageformat::TsumugiRescueReadEvidence rescue_read_evidence() {
  return ytec::imageformat::TsumugiRescueReadEvidence{
      .forward_attempts = 1U,
      .reverse_attempts = 1U,
      .sector_attempts = 1U,
      .zero_fill_read_back_verified = true,
      .forward_native_error = ERROR_CRC,
      .reverse_native_error = ERROR_SECTOR_NOT_FOUND,
      .sector_native_error = ERROR_READ_FAULT,
  };
}

ytec::imageformat::TsumugiBuildRequest sample_request() {
  ytec::imageformat::TsumugiBuildRequest request{};
  request.payload_kind = ytec::imageformat::TsumugiPayloadKind::exact_disk;
  request.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  request.logical_sector_size = 512U;
  request.physical_sector_size = 4096U;
  request.chunk_size = ytec::imageformat::kImageChunkSize16MiB;
  request.compression = ytec::imageformat::ImageCompression::zstandard;
  for (std::size_t index = 0; index < request.image_id.size(); ++index) {
    request.image_id[index] = static_cast<std::byte>(index + 1U);
  }
  request.manifest = {
      std::byte{'T'}, std::byte{'S'}, std::byte{'M'}, std::byte{'1'},
      std::byte{'M'}, std::byte{'A'}, std::byte{'N'}, std::byte{'1'}};

  ytec::imageformat::TsumugiBuildChunk first{};
  first.logical_offset = 0U;
  first.logical_length = 4096U;
  first.data.assign(4096U, std::byte{0x5A});
  request.chunks.push_back(std::move(first));

  ytec::imageformat::TsumugiBuildChunk zero{};
  zero.logical_offset = 16ULL * 1024ULL * 1024ULL;
  zero.logical_length = 8192U;
  zero.flags = ytec::imageformat::TsumugiChunkFlags::zero_filled;
  request.chunks.push_back(std::move(zero));

  ytec::imageformat::TsumugiBuildChunk last{};
  last.logical_offset = 32ULL * 1024ULL * 1024ULL;
  last.logical_length = 4096U;
  last.data.resize(4096U);
  std::uint32_t state = 0x13579BDFU;
  for (auto& byte : last.data) {
    state = state * 1664525U + 1013904223U;
    byte = static_cast<std::byte>(state & 0xFFU);
  }
  request.chunks.push_back(std::move(last));
  return request;
}

ytec::imageformat::TsumugiEncryptionSettings encryption_settings() {
  ytec::imageformat::TsumugiEncryptionSettings settings{};
  settings.password = "Tsumugi-Drive-2026!";
  for (std::size_t index = 0; index < settings.argon2.salt.size(); ++index) {
    settings.argon2.salt[index] =
        static_cast<std::byte>(0x20U + index);
  }
  for (std::size_t index = 0; index < settings.base_nonce.size(); ++index) {
    settings.base_nonce[index] =
        static_cast<std::byte>(0x80U + index);
  }
  return settings;
}

void test_unencrypted_round_trip_and_compression() {
  const auto request = sample_request();
  const auto built = ytec::imageformat::build_tsumugi_v1(request);
  check(built.has_value(), "A valid unencrypted .tsumugi must build");
  const auto inspected = ytec::imageformat::inspect_tsumugi_v1(built.value());
  if (!inspected) {
    print_error(inspected.error());
  }
  check(inspected.has_value(), "A freshly built .tsumugi must verify");
  check(inspected.value().manifest == request.manifest,
        "Verified manifest bytes must round-trip exactly");
  check(inspected.value().chunks.size() == request.chunks.size(),
        "Every chunk record must round-trip");
  check(inspected.value().chunks[0].plaintext == request.chunks[0].data,
        "Compressed plaintext must verify and expand exactly");
  check(
      inspected.value().chunks[0].record.compression ==
          ytec::imageformat::ImageCompression::zstandard,
      "Compressible chunks should use Zstandard when it is smaller");
  check(
      inspected.value().chunks[1].record.flags ==
          ytec::imageformat::TsumugiChunkFlags::zero_filled &&
          inspected.value().chunks[1].plaintext.empty(),
      "Zero ranges must remain implicit");
  check(inspected.value().header_hash_verified &&
            inspected.value().all_chunks_verified &&
            inspected.value().global_hash_verified,
        "All integrity layers must report verified");
}

void test_encrypted_round_trip_and_authentication() {
  auto request = sample_request();
  const auto settings = encryption_settings();
  request.encryption = settings;
  const auto built = ytec::imageformat::build_tsumugi_v1(request);
  check(built.has_value(), "A valid encrypted .tsumugi must build");
  check(
      !ytec::imageformat::inspect_tsumugi_v1(built.value()).has_value(),
      "Encrypted metadata must not be exposed without a password");
  check(
      !ytec::imageformat::inspect_tsumugi_v1(
           built.value(), {.password = "Wrong-password-2026!"})
           .has_value(),
      "A wrong password must fail authentication");
  const auto inspected = ytec::imageformat::inspect_tsumugi_v1(
      built.value(), {.password = settings.password});
  if (!inspected) {
    print_error(inspected.error());
  }
  check(inspected.has_value(), "The correct password must verify the image");
  check(inspected.value().metadata_authenticated &&
            inspected.value().chunks[0].plaintext == request.chunks[0].data,
        "Metadata and payload must authenticate before exposure");

  auto changed_metadata = built.value();
  const auto metadata_offset =
      read_little<std::uint64_t>(changed_metadata, 80U);
  changed_metadata[static_cast<std::size_t>(metadata_offset)] ^=
      std::byte{0x01};
  refresh_global_hash(changed_metadata);
  check(
      !ytec::imageformat::inspect_tsumugi_v1(
           changed_metadata, {.password = settings.password})
           .has_value(),
      "Encrypted metadata tampering must fail even with a refreshed outer hash");

  auto changed_payload = built.value();
  const auto data_offset = read_little<std::uint64_t>(changed_payload, 64U);
  changed_payload[static_cast<std::size_t>(data_offset)] ^=
      std::byte{0x40};
  refresh_global_hash(changed_payload);
  check(
      !ytec::imageformat::inspect_tsumugi_v1(
           changed_payload, {.password = settings.password})
           .has_value(),
      "Encrypted payload tampering must fail its per-chunk GCM tag");
}

void test_untrusted_bounds_and_required_features_fail_closed() {
  const auto built = ytec::imageformat::build_tsumugi_v1(sample_request());
  check(built.has_value(), "The synthetic image must build");

  auto truncated = built.value();
  truncated.pop_back();
  check(!ytec::imageformat::inspect_tsumugi_v1(truncated).has_value(),
        "A truncated footer must fail closed");

  auto unknown_feature = built.value();
  write_little<std::uint32_t>(unknown_feature, 20U, 0x80000000U);
  check(!ytec::imageformat::inspect_tsumugi_v1(unknown_feature).has_value(),
        "An unknown required feature flag must be rejected");

  auto reserved = built.value();
  reserved[300U] = std::byte{0x01};
  check(!ytec::imageformat::inspect_tsumugi_v1(reserved).has_value(),
        "Non-zero reserved header bytes must be rejected");

  auto overflow = built.value();
  write_little<std::uint64_t>(overflow, 64U,
                              UINT64_MAX - 1024ULL);
  check(!ytec::imageformat::inspect_tsumugi_v1(overflow).has_value(),
        "Overflowing section offsets must be rejected");
}

void test_encryption_requires_per_image_random_material() {
  auto request = sample_request();
  auto settings = encryption_settings();
  settings.argon2.salt.fill(std::byte{0});
  request.encryption = settings;
  check(!ytec::imageformat::build_tsumugi_v1(request).has_value(),
        "An all-zero Argon2 salt must be rejected");

  settings = encryption_settings();
  settings.base_nonce.fill(std::byte{0});
  request.encryption = settings;
  check(!ytec::imageformat::build_tsumugi_v1(request).has_value(),
        "An all-zero base nonce must be rejected");
}

void test_rescue_map_is_explicit_and_mode_bound() {
  auto normal = sample_request();
  normal.chunks[1].flags =
      ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled;
  check(!ytec::imageformat::build_tsumugi_v1(normal).has_value(),
        "Unreadable ranges must be rejected outside rescue mode");

  auto rescue = sample_request();
  rescue.payload_kind = ytec::imageformat::TsumugiPayloadKind::rescue_disk;
  rescue.compression = ytec::imageformat::ImageCompression::none;
  rescue.chunks[1].flags =
      ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled;
  const auto evidence = rescue_read_evidence();
  rescue.chunks[1].rescue_read_evidence = evidence;
  const auto built = ytec::imageformat::build_tsumugi_v1(rescue);
  check(built.has_value(), "A rescue image with an explicit gap must build");
  const auto inspected = ytec::imageformat::inspect_tsumugi_v1(built.value());
  if (!inspected) {
    print_error(inspected.error());
  }
  check(inspected.has_value(), "A rescue image must verify");
  check(
      inspected.value().header.payload_kind ==
              ytec::imageformat::TsumugiPayloadKind::rescue_disk &&
          inspected.value().chunks[1].record.flags ==
              ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled &&
          inspected.value().chunks[1].record.rescue_read_evidence == evidence &&
          (inspected.value().header.required_features &
           static_cast<std::uint32_t>(
               ytec::imageformat::TsumugiRequiredFeature::
                   rescue_read_evidence)) != 0U,
      "The verified result must preserve its warning map and retry evidence");

  auto missing_evidence = rescue;
  missing_evidence.chunks[1].rescue_read_evidence.reset();
  check(!ytec::imageformat::build_tsumugi_v1(missing_evidence).has_value(),
        "A new rescue image must not omit finite retry evidence");

  auto unverified_zero_fill = rescue;
  unverified_zero_fill.chunks[1]
      .rescue_read_evidence->zero_fill_read_back_verified = false;
  check(!ytec::imageformat::build_tsumugi_v1(unverified_zero_fill).has_value(),
        "An unreadable range without verified zero-fill must be rejected");

  const std::uint64_t metadata_offset =
      read_little<std::uint64_t>(built.value(), 80U);
  const std::size_t second_record_offset =
      static_cast<std::size_t>(metadata_offset) +
      ytec::imageformat::kTsumugiMetadataHeaderSize +
      rescue.manifest.size() + ytec::imageformat::kTsumugiChunkRecordSize;

  auto tampered_evidence = built.value();
  tampered_evidence[second_record_offset + 96U] = std::byte{0};
  refresh_global_hash(tampered_evidence);
  check(!ytec::imageformat::inspect_tsumugi_v1(tampered_evidence).has_value(),
        "A refreshed outer hash must not legitimize invalid retry evidence");

  auto legacy_rescue = built.value();
  write_little<std::uint32_t>(
      legacy_rescue,
      20U,
      read_little<std::uint32_t>(legacy_rescue, 20U) & ~4U);
  std::fill(
      legacy_rescue.begin() +
          static_cast<std::ptrdiff_t>(second_record_offset + 96U),
      legacy_rescue.begin() +
          static_cast<std::ptrdiff_t>(second_record_offset + 112U),
      std::byte{0});
  refresh_header_hash(legacy_rescue);
  refresh_global_hash(legacy_rescue);
  const auto legacy_inspected =
      ytec::imageformat::inspect_tsumugi_v1(legacy_rescue);
  check(legacy_inspected.has_value() &&
            !legacy_inspected.value().chunks[1].record
                 .rescue_read_evidence.has_value(),
        "A pre-extension rescue image must remain readable without evidence");

  auto rescue_without_gap = sample_request();
  rescue_without_gap.payload_kind =
      ytec::imageformat::TsumugiPayloadKind::rescue_disk;
  const auto lossless_rescue =
      ytec::imageformat::build_tsumugi_v1(rescue_without_gap);
  check(lossless_rescue.has_value(),
        "A fully recovered rescue run must retain its rescue classification");
  const auto lossless_inspected =
      ytec::imageformat::inspect_tsumugi_v1(lossless_rescue.value());
  check(lossless_inspected.has_value() &&
            lossless_inspected.value().header.payload_kind ==
                ytec::imageformat::TsumugiPayloadKind::rescue_disk &&
            (lossless_inspected.value().header.required_features & 6U) == 0U,
        "A lossless rescue must not forge an unreadable map or retry evidence");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"unencrypted_round_trip_and_compression",
       test_unencrypted_round_trip_and_compression},
      {"encrypted_round_trip_and_authentication",
       test_encrypted_round_trip_and_authentication},
      {"untrusted_bounds_and_required_features_fail_closed",
       test_untrusted_bounds_and_required_features_fail_closed},
      {"encryption_requires_per_image_random_material",
       test_encryption_requires_per_image_random_material},
      {"rescue_map_is_explicit_and_mode_bound",
       test_rescue_map_is_explicit_and_mode_bound},
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

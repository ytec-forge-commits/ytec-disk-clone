#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_manifest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

namespace {

constexpr std::size_t kMaximumInputBytes = 64U * 1024U;
constexpr std::size_t kEncryptedGoldenBytes = 6'096U;
constexpr std::string_view kGoldenPassword = "Golden-Tsumugi-v1!";
constexpr ytec::imageformat::Sha256Digest kEncryptedGoldenHash{
    std::byte{0x43}, std::byte{0x62}, std::byte{0xAE}, std::byte{0x1B},
    std::byte{0xC9}, std::byte{0x21}, std::byte{0xD0}, std::byte{0x6D},
    std::byte{0x08}, std::byte{0xDF}, std::byte{0xBE}, std::byte{0x1A},
    std::byte{0x74}, std::byte{0xD9}, std::byte{0x57}, std::byte{0xF9},
    std::byte{0xF5}, std::byte{0xA0}, std::byte{0x99}, std::byte{0x0A},
    std::byte{0xB3}, std::byte{0x95}, std::byte{0x32}, std::byte{0x0E},
    std::byte{0x89}, std::byte{0xB2}, std::byte{0x54}, std::byte{0xEA},
    std::byte{0x77}, std::byte{0xC0}, std::byte{0x8F}, std::byte{0xD3},
};

[[noreturn]] void fail_invariant() noexcept {
  std::abort();
}

bool is_fixed_encrypted_golden(
    const std::span<const std::byte> bytes) {
  if (bytes.size() != kEncryptedGoldenBytes) {
    return false;
  }
  const auto digest = ytec::imageformat::sha256(bytes);
  return digest.has_value() && digest.value() == kEncryptedGoldenHash;
}

bool encrypted_golden_preflight_enabled() noexcept {
  // Argon2id deliberately allocates a large bounded working set. Repeating
  // that deterministic success vector millions of times under ASan can
  // exhaust the sanitizer allocator/quarantine without finding a parser bug.
  // The runner enables this branch in a dedicated one-input ASan preflight,
  // then starts the coverage-guided process without the environment switch.
  static const bool enabled = []() noexcept {
    char* value = nullptr;
    std::size_t length = 0U;
    const auto error = _dupenv_s(
        &value, &length, "YTEC_IMAGE_FUZZ_ENABLE_ENCRYPTED_GOLDEN");
    const bool present = error == 0 && value != nullptr && length != 0U;
    std::free(value);
    return present;
  }();
  return enabled;
}

template <typename Container>
void require_canonical_bytes(
    const Container& rebuilt,
    const std::span<const std::byte> input) {
  if (rebuilt.size() != input.size() ||
      !std::equal(rebuilt.begin(), rebuilt.end(), input.begin())) {
    fail_invariant();
  }
}

void exercise_tsumugi(const std::span<const std::byte> bytes) {
  using namespace ytec::imageformat;
  auto inspected = inspect_tsumugi_v1(bytes);
  if (!inspected.has_value() && encrypted_golden_preflight_enabled() &&
      is_fixed_encrypted_golden(bytes)) {
    inspected = inspect_tsumugi_v1(bytes, {.password = kGoldenPassword});
  }
  if (!inspected.has_value()) {
    return;
  }
  if (!inspected.value().header_hash_verified ||
      !inspected.value().all_chunks_verified ||
      !inspected.value().global_hash_verified) {
    fail_invariant();
  }
}

void exercise_manifest(const std::span<const std::byte> bytes) {
  using namespace ytec::imageformat;
  const auto inspected = inspect_tsumugi_manifest_v1(bytes);
  if (!inspected.has_value()) {
    return;
  }
  const auto rebuilt = build_tsumugi_manifest_v1(inspected.value());
  if (!rebuilt.has_value()) {
    fail_invariant();
  }
  require_canonical_bytes(rebuilt.value(), bytes);
}

void exercise_snapshot(const std::span<const std::byte> bytes) {
  using namespace ytec::imageformat;
  const auto inspected = inspect_partition_snapshot_v1(bytes);
  if (!inspected.has_value()) {
    return;
  }
  const auto rebuilt = build_partition_snapshot_v1(inspected.value());
  if (!rebuilt.has_value()) {
    fail_invariant();
  }
  require_canonical_bytes(rebuilt.value(), bytes);
}

}  // namespace

// The MSVC 19.50 bundled libFuzzer runtime probes the host executable for the
// optional MemorySanitizer interface even when this target is built without
// MemorySanitizer (which MSVC does not provide on Windows).  Export explicit
// no-op hooks so that the probe can distinguish "not instrumented with MSan"
// from a missing host symbol.  These hooks do not replace AddressSanitizer or
// coverage instrumentation; both are separate compiler/runtime facilities.
extern "C" __declspec(dllexport) void
__msan_scoped_disable_interceptor_checks() noexcept {}

extern "C" __declspec(dllexport) void
__msan_scoped_enable_interceptor_checks() noexcept {}

extern "C" __declspec(dllexport) void __msan_unpoison(
    const volatile void*, const std::size_t) noexcept {}

extern "C" __declspec(dllexport) void __msan_unpoison_param(
    const std::size_t) noexcept {}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* const data,
    const std::size_t size) {
  if (size > kMaximumInputBytes) {
    return 0;
  }
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data), size);
  exercise_tsumugi(bytes);
  exercise_manifest(bytes);
  exercise_snapshot(bytes);
  return 0;
}

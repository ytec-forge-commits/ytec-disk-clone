#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint32_t kTsumugiArgon2MemoryKiB = 64U * 1024U;
inline constexpr std::uint32_t kTsumugiArgon2Iterations = 3U;
inline constexpr std::uint32_t kTsumugiArgon2Parallelism = 1U;
inline constexpr std::size_t kTsumugiSaltBytes = 16U;
inline constexpr std::size_t kTsumugiKeyBytes = 32U;
inline constexpr std::size_t kTsumugiGcmNonceBytes = 12U;
inline constexpr std::size_t kTsumugiGcmTagBytes = 16U;
inline constexpr std::size_t kTsumugiMaximumCryptBytes =
    32U * 1024U * 1024U;

struct TsumugiArgon2Parameters final {
  std::uint32_t memory_kib{kTsumugiArgon2MemoryKiB};
  std::uint32_t iterations{kTsumugiArgon2Iterations};
  std::uint32_t parallelism{kTsumugiArgon2Parallelism};
  std::array<std::byte, kTsumugiSaltBytes> salt{};
};

struct TsumugiPasswordAssessment final {
  bool accepted{};
  bool weak{};
};

class TsumugiKey final {
 public:
  TsumugiKey() = default;
  explicit TsumugiKey(std::array<std::byte, kTsumugiKeyBytes> bytes) noexcept;
  ~TsumugiKey();

  TsumugiKey(const TsumugiKey&) = delete;
  TsumugiKey& operator=(const TsumugiKey&) = delete;
  TsumugiKey(TsumugiKey&& other) noexcept;
  TsumugiKey& operator=(TsumugiKey&& other) noexcept;

  [[nodiscard]] std::span<const std::byte, kTsumugiKeyBytes> bytes() const
      noexcept;

 private:
  void clear() noexcept;
  std::array<std::byte, kTsumugiKeyBytes> bytes_{};
};

struct TsumugiEncryptedBytes final {
  std::vector<std::byte> ciphertext;
  std::array<std::byte, kTsumugiGcmTagBytes> tag{};
};

[[nodiscard]] TsumugiPasswordAssessment assess_tsumugi_password(
    std::string_view password) noexcept;

[[nodiscard]] clonecore::Result<
    std::array<std::byte, kTsumugiSaltBytes>>
generate_tsumugi_salt();

[[nodiscard]] clonecore::Result<
    std::array<std::byte, kTsumugiGcmNonceBytes>>
generate_tsumugi_nonce();

// The public product profile is fixed to Argon2id v1.3, 64 MiB, t=3, p=1.
// The parameterized form exists so the official known-answer vector can be
// tested and so future format versions can read their recorded profile.
[[nodiscard]] clonecore::Result<TsumugiKey> derive_tsumugi_key_argon2id(
    std::string_view password,
    const TsumugiArgon2Parameters& parameters);

[[nodiscard]] clonecore::Result<TsumugiEncryptedBytes>
encrypt_tsumugi_aes256_gcm(
    const TsumugiKey& key,
    std::span<const std::byte, kTsumugiGcmNonceBytes> nonce,
    std::span<const std::byte> additional_authenticated_data,
    std::span<const std::byte> plaintext);

[[nodiscard]] clonecore::Result<std::vector<std::byte>>
decrypt_tsumugi_aes256_gcm(
    const TsumugiKey& key,
    std::span<const std::byte, kTsumugiGcmNonceBytes> nonce,
    std::span<const std::byte> additional_authenticated_data,
    std::span<const std::byte> ciphertext,
    std::span<const std::byte, kTsumugiGcmTagBytes> tag);

}  // namespace ytec::imageformat

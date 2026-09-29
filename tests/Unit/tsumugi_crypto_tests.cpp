#include "argon2.h"

#include "ytec/imageformat/tsumugi_crypto.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
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

std::string to_hex(const std::span<const std::byte> bytes) {
  constexpr std::array<char, 16> digits{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const std::byte value : bytes) {
    const auto number = std::to_integer<unsigned int>(value);
    result.push_back(digits[(number >> 4U) & 0x0FU]);
    result.push_back(digits[number & 0x0FU]);
  }
  return result;
}

void test_password_policy() {
  using ytec::imageformat::assess_tsumugi_password;
  check(
      !assess_tsumugi_password("short").accepted,
      "Passwords shorter than eight characters must be rejected");
  check(
      !assess_tsumugi_password("valid\npassword").accepted,
      "Non-printable ASCII must be rejected");
  const auto weak = assess_tsumugi_password("abcdefgh");
  check(weak.accepted && weak.weak,
        "A weak printable password remains usable with a warning");
  const auto strong = assess_tsumugi_password("Tsumugi-Drive-2026!");
  check(strong.accepted && !strong.weak,
        "A long mixed-class password should not be marked weak");
}

void test_official_argon2id_known_answer() {
  // Argon2 reference implementation 20190702, src/test.c, v=19 vector.
  constexpr std::string_view password = "password";
  constexpr std::string_view salt = "somesalt";
  std::array<std::byte, 32> output{};
  const int status = argon2id_hash_raw(
      2U,
      65'536U,
      1U,
      password.data(),
      password.size(),
      salt.data(),
      salt.size(),
      output.data(),
      output.size());
  check(status == ARGON2_OK, "The official Argon2id vector must execute");
  check(
      to_hex(output) ==
          "09316115d5cf24ed5a15a31a3ba326e5"
          "cf32edc24702987c02b6566f61913cf7",
      "Argon2id v1.3 output must match the official known-answer vector");
}

void test_product_profile_is_deterministic_and_salt_bound() {
  ytec::imageformat::TsumugiArgon2Parameters first{};
  ytec::imageformat::TsumugiArgon2Parameters second{};
  for (std::size_t index = 0; index < first.salt.size(); ++index) {
    first.salt[index] = static_cast<std::byte>(index + 1U);
    second.salt[index] = static_cast<std::byte>(index + 2U);
  }
  auto first_key = ytec::imageformat::derive_tsumugi_key_argon2id(
      "Tsumugi-Drive-2026!", first);
  auto same_key = ytec::imageformat::derive_tsumugi_key_argon2id(
      "Tsumugi-Drive-2026!", first);
  auto second_key = ytec::imageformat::derive_tsumugi_key_argon2id(
      "Tsumugi-Drive-2026!", second);
  check(first_key && same_key && second_key,
        "The fixed product Argon2id profile must derive keys");
  check(std::equal(
            first_key.value().bytes().begin(),
            first_key.value().bytes().end(),
            same_key.value().bytes().begin()),
        "The same password and salt must derive the same key");
  check(!std::equal(
            first_key.value().bytes().begin(),
            first_key.value().bytes().end(),
            second_key.value().bytes().begin()),
        "A per-image salt must change the derived key");
}

void test_aes_gcm_round_trip_and_tamper_rejection() {
  ytec::imageformat::TsumugiArgon2Parameters parameters{};
  for (std::size_t index = 0; index < parameters.salt.size(); ++index) {
    parameters.salt[index] = static_cast<std::byte>(0x40U + index);
  }
  auto key = ytec::imageformat::derive_tsumugi_key_argon2id(
      "Tsumugi-Drive-2026!", parameters);
  check(key.has_value(), "A valid key must be available for AES-GCM tests");

  std::array<std::byte, ytec::imageformat::kTsumugiGcmNonceBytes> nonce{};
  nonce[0] = std::byte{0xA5};
  const std::array<std::byte, 8> aad{
      std::byte{'T'}, std::byte{'S'}, std::byte{'U'}, std::byte{'M'},
      std::byte{'U'}, std::byte{'G'}, std::byte{'I'}, std::byte{'1'}};
  const std::vector<std::byte> plaintext{
      std::byte{0x00}, std::byte{0x11}, std::byte{0x22},
      std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};
  auto encrypted = ytec::imageformat::encrypt_tsumugi_aes256_gcm(
      key.value(), nonce, aad, plaintext);
  check(encrypted.has_value(), "AES-256-GCM encryption must succeed");
  auto decrypted = ytec::imageformat::decrypt_tsumugi_aes256_gcm(
      key.value(), nonce, aad, encrypted.value().ciphertext,
      encrypted.value().tag);
  check(decrypted && decrypted.value() == plaintext,
        "AES-256-GCM must round-trip exact bytes");

  auto changed_ciphertext = encrypted.value().ciphertext;
  changed_ciphertext.front() ^= std::byte{0x01};
  check(
      !ytec::imageformat::decrypt_tsumugi_aes256_gcm(
           key.value(), nonce, aad, changed_ciphertext,
           encrypted.value().tag),
      "Ciphertext tampering must fail authentication");
  auto changed_tag = encrypted.value().tag;
  changed_tag.back() ^= std::byte{0x80};
  check(
      !ytec::imageformat::decrypt_tsumugi_aes256_gcm(
           key.value(), nonce, aad, encrypted.value().ciphertext,
           changed_tag),
      "Tag tampering must fail authentication");
  auto changed_aad = aad;
  changed_aad[0] ^= std::byte{0x20};
  check(
      !ytec::imageformat::decrypt_tsumugi_aes256_gcm(
           key.value(), nonce, changed_aad,
           encrypted.value().ciphertext, encrypted.value().tag),
      "AAD tampering must fail authentication");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"password_policy", test_password_policy},
      {"official_argon2id_known_answer",
       test_official_argon2id_known_answer},
      {"product_profile_is_deterministic_and_salt_bound",
       test_product_profile_is_deterministic_and_salt_bound},
      {"aes_gcm_round_trip_and_tamper_rejection",
       test_aes_gcm_round_trip_and_tamper_rejection},
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

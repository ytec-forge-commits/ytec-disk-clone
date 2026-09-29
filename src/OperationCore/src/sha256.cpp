#include "sha256_internal.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace ytec::operationcore::detail {
namespace {

clonecore::Error hash_error(const NTSTATUS status) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::verification_failed,
      .native_code = static_cast<DWORD>(status),
      .operation = L"OperationCore SHA-256",
      .message = L"Windows CNGによるSHA-256計算に失敗しました",
  };
}

class AlgorithmHandle final {
 public:
  AlgorithmHandle() = default;
  ~AlgorithmHandle() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }

  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  AlgorithmHandle(AlgorithmHandle&&) = delete;
  AlgorithmHandle& operator=(AlgorithmHandle&&) = delete;

  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class HashHandle final {
 public:
  HashHandle() = default;
  ~HashHandle() {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }

  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  HashHandle(HashHandle&&) = delete;
  HashHandle& operator=(HashHandle&&) = delete;

  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

}  // namespace

clonecore::Result<Sha256Digest> sha256(
    const std::span<const std::byte> bytes) {
  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<Sha256Digest>::failure(hash_error(status));
  }

  ULONG object_length{};
  ULONG returned{};
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_length) ||
      object_length == 0U) {
    return clonecore::Result<Sha256Digest>::failure(hash_error(status));
  }

  ULONG hash_length{};
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&hash_length),
      sizeof(hash_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(hash_length) ||
      hash_length != Sha256Digest{}.size()) {
    return clonecore::Result<Sha256Digest>::failure(hash_error(status));
  }

  std::vector<UCHAR> object(object_length);
  HashHandle hash;
  status = BCryptCreateHash(
      algorithm.get(),
      hash.put(),
      object.data(),
      object_length,
      nullptr,
      0,
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<Sha256Digest>::failure(hash_error(status));
  }

  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const ULONG amount = static_cast<ULONG>(std::min<std::size_t>(
        bytes.size() - consumed,
        std::numeric_limits<ULONG>::max()));
    status = BCryptHashData(
        hash.get(),
        reinterpret_cast<PUCHAR>(
            const_cast<std::byte*>(bytes.data() + consumed)),
        amount,
        0);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Result<Sha256Digest>::failure(hash_error(status));
    }
    consumed += amount;
  }

  Sha256Digest digest{};
  status = BCryptFinishHash(
      hash.get(),
      reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<Sha256Digest>::failure(hash_error(status));
  }
  return clonecore::Result<Sha256Digest>::success(digest);
}

bool digest_is_zero(const Sha256Digest& digest) noexcept {
  unsigned int combined = 0U;
  for (const std::byte value : digest) {
    combined |= std::to_integer<unsigned int>(value);
  }
  return combined == 0U;
}

bool digest_equal(
    const Sha256Digest& left,
    const Sha256Digest& right) noexcept {
  unsigned int difference = 0U;
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

}  // namespace ytec::operationcore::detail

#include "ytec/imageformat/sha256.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error hash_error(
    const std::wstring_view operation,
    const NTSTATUS status) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::verification_failed,
      .native_code = static_cast<DWORD>(status),
      .operation = std::wstring(operation),
      .message = L"Windows CNG SHA-256処理に失敗しました",
  };
}

class AlgorithmHandle final {
 public:
  ~AlgorithmHandle() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }

  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  AlgorithmHandle(AlgorithmHandle&&) = delete;
  AlgorithmHandle& operator=(AlgorithmHandle&&) = delete;

  AlgorithmHandle() = default;

  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class HashHandle final {
 public:
  ~HashHandle() {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }

  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  HashHandle(HashHandle&&) = delete;
  HashHandle& operator=(HashHandle&&) = delete;

  HashHandle() = default;

  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

class CngSha256 final {
 public:
  [[nodiscard]] clonecore::Status initialize() {
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        algorithm_.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Status::failure(
          hash_error(L"SHA-256アルゴリズム初期化", status));
    }

    ULONG returned = 0;
    status = BCryptGetProperty(
        algorithm_.get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_length_),
        sizeof(object_length_),
        &returned,
        0);
    if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_length_) ||
        object_length_ == 0) {
      return clonecore::Status::failure(
          hash_error(L"SHA-256オブジェクト長取得", status));
    }

    ULONG hash_length = 0;
    status = BCryptGetProperty(
        algorithm_.get(),
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hash_length),
        sizeof(hash_length),
        &returned,
        0);
    if (!BCRYPT_SUCCESS(status) || returned != sizeof(hash_length) ||
        hash_length != Sha256Digest{}.size()) {
      return clonecore::Status::failure(
          hash_error(L"SHA-256出力長取得", status));
    }

    object_.resize(object_length_);
    status = BCryptCreateHash(
        algorithm_.get(),
        hash_.put(),
        object_.data(),
        object_length_,
        nullptr,
        0,
        0);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Status::failure(
          hash_error(L"SHA-256ハッシュ生成", status));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status update(
      const std::span<const std::byte> bytes) {
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
      const std::size_t remaining = bytes.size() - consumed;
      const ULONG length = static_cast<ULONG>(std::min<std::size_t>(
          remaining, std::numeric_limits<ULONG>::max()));
      const NTSTATUS status = BCryptHashData(
          hash_.get(),
          reinterpret_cast<PUCHAR>(
              const_cast<std::byte*>(bytes.data() + consumed)),
          length,
          0);
      if (!BCRYPT_SUCCESS(status)) {
        return clonecore::Status::failure(
            hash_error(L"SHA-256データ処理", status));
      }
      consumed += length;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<Sha256Digest> finish() {
    Sha256Digest digest{};
    const NTSTATUS status = BCryptFinishHash(
        hash_.get(),
        reinterpret_cast<PUCHAR>(digest.data()),
        static_cast<ULONG>(digest.size()),
        0);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Result<Sha256Digest>::failure(
          hash_error(L"SHA-256完了", status));
    }
    return clonecore::Result<Sha256Digest>::success(digest);
  }

 private:
  // BCryptDestroyHash may still access the caller-owned hash object buffer.
  // Members are destroyed in reverse declaration order, so keep hash_ last:
  // hash handle -> backing buffer -> algorithm provider.
  AlgorithmHandle algorithm_;
  ULONG object_length_{};
  std::vector<UCHAR> object_;
  HashHandle hash_;
};

}  // namespace

clonecore::Result<Sha256Digest> sha256(
    const std::span<const std::byte> bytes) {
  CngSha256 hasher;
  const auto initialized = hasher.initialize();
  if (!initialized) {
    return clonecore::Result<Sha256Digest>::failure(initialized.error());
  }
  const auto updated = hasher.update(bytes);
  if (!updated) {
    return clonecore::Result<Sha256Digest>::failure(updated.error());
  }
  return hasher.finish();
}

clonecore::Result<Sha256Digest> sha256_zeroes(const std::uint64_t length) {
  CngSha256 hasher;
  const auto initialized = hasher.initialize();
  if (!initialized) {
    return clonecore::Result<Sha256Digest>::failure(initialized.error());
  }
  constexpr std::size_t kZeroBufferSize = 1024U * 1024U;
  const std::vector<std::byte> zeroes(kZeroBufferSize, std::byte{0});
  std::uint64_t remaining = length;
  while (remaining > 0) {
    const std::size_t amount = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, zeroes.size()));
    const auto updated = hasher.update(
        std::span<const std::byte>(zeroes.data(), amount));
    if (!updated) {
      return clonecore::Result<Sha256Digest>::failure(updated.error());
    }
    remaining -= amount;
  }
  return hasher.finish();
}

clonecore::Result<Sha256Digest> sha256_from_reader(
    const std::uint64_t length,
    const std::size_t maximum_block_bytes,
    const Sha256ReadCallback& reader) {
  constexpr std::size_t kMaximumReadBlock = 32U * 1024U * 1024U;
  if (maximum_block_bytes == 0 ||
      maximum_block_bytes > kMaximumReadBlock ||
      !reader) {
    return clonecore::Result<Sha256Digest>::failure(clonecore::Error{
        .code = clonecore::ErrorCode::invalid_argument,
        .native_code = ERROR_INVALID_PARAMETER,
        .operation = L"SHA-256読戻し設定",
        .message = L"読戻しブロック寸法またはCallbackが不正です",
    });
  }

  CngSha256 hasher;
  const auto initialized = hasher.initialize();
  if (!initialized) {
    return clonecore::Result<Sha256Digest>::failure(initialized.error());
  }

  std::uint64_t offset = 0;
  while (offset < length) {
    const std::size_t amount = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            length - offset,
            maximum_block_bytes));
    auto block = reader(offset, amount);
    if (!block) {
      return clonecore::Result<Sha256Digest>::failure(block.error());
    }
    if (block.value().size() != amount) {
      return clonecore::Result<Sha256Digest>::failure(clonecore::Error{
          .code = clonecore::ErrorCode::verification_failed,
          .native_code = ERROR_HANDLE_EOF,
          .operation = L"SHA-256読戻し長",
          .message = L"Callbackが要求長と異なるデータを返しました",
      });
    }
    const auto updated = hasher.update(block.value());
    if (!updated) {
      return clonecore::Result<Sha256Digest>::failure(updated.error());
    }
    offset += amount;
  }
  return hasher.finish();
}

}  // namespace ytec::imageformat

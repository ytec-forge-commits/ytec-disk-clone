#include "ytec/imageformat/tsumugi_crypto.h"

#include <Windows.h>
#include <bcrypt.h>

#include <argon2.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace ytec::imageformat {
namespace {

clonecore::Error crypto_error(
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

class AlgorithmHandle final {
 public:
  ~AlgorithmHandle() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }
  AlgorithmHandle() = default;
  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  AlgorithmHandle(AlgorithmHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  AlgorithmHandle& operator=(AlgorithmHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        BCryptCloseAlgorithmProvider(handle_, 0);
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class KeyHandle final {
 public:
  ~KeyHandle() {
    if (handle_ != nullptr) {
      BCryptDestroyKey(handle_);
    }
    if (!object_.empty()) {
      SecureZeroMemory(object_.data(), object_.size());
    }
  }
  KeyHandle() = default;
  KeyHandle(const KeyHandle&) = delete;
  KeyHandle& operator=(const KeyHandle&) = delete;
  KeyHandle(KeyHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        object_(std::move(other.object_)) {}
  KeyHandle& operator=(KeyHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        BCryptDestroyKey(handle_);
      }
      if (!object_.empty()) {
        SecureZeroMemory(object_.data(), object_.size());
      }
      handle_ = std::exchange(other.handle_, nullptr);
      object_ = std::move(other.object_);
    }
    return *this;
  }
  [[nodiscard]] BCRYPT_KEY_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] std::vector<UCHAR>& object() noexcept { return object_; }

 private:
  BCRYPT_KEY_HANDLE handle_{};
  std::vector<UCHAR> object_;
};

clonecore::Result<std::pair<AlgorithmHandle, KeyHandle>> open_aes_key(
    const TsumugiKey& key) {
  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_AES_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<
        std::pair<AlgorithmHandle, KeyHandle>>::failure(crypto_error(
            clonecore::ErrorCode::internal_error,
            static_cast<DWORD>(status),
            L"AES-256-GCMアルゴリズム初期化",
            L"Windows CNGのAES Providerを初期化できませんでした"));
  }
  status = BCryptSetProperty(
      algorithm.get(),
      BCRYPT_CHAINING_MODE,
      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
      static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<
        std::pair<AlgorithmHandle, KeyHandle>>::failure(crypto_error(
            clonecore::ErrorCode::internal_error,
            static_cast<DWORD>(status),
            L"AES-256-GCMモード設定",
            L"Windows CNGをGCMモードへ設定できませんでした"));
  }

  ULONG object_length = 0;
  ULONG returned = 0;
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_length) ||
      object_length == 0) {
    return clonecore::Result<
        std::pair<AlgorithmHandle, KeyHandle>>::failure(crypto_error(
            clonecore::ErrorCode::internal_error,
            static_cast<DWORD>(status),
            L"AES-256-GCM鍵領域確認",
            L"Windows CNGの鍵領域寸法を確認できませんでした"));
  }

  KeyHandle symmetric_key;
  symmetric_key.object().resize(object_length);
  status = BCryptGenerateSymmetricKey(
      algorithm.get(),
      symmetric_key.put(),
      symmetric_key.object().data(),
      object_length,
      reinterpret_cast<PUCHAR>(
          const_cast<std::byte*>(key.bytes().data())),
      static_cast<ULONG>(key.bytes().size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<
        std::pair<AlgorithmHandle, KeyHandle>>::failure(crypto_error(
            clonecore::ErrorCode::internal_error,
            static_cast<DWORD>(status),
            L"AES-256-GCM鍵生成",
            L"Windows CNGの対称鍵を生成できませんでした"));
  }
  return clonecore::Result<std::pair<AlgorithmHandle, KeyHandle>>::success(
      std::pair<AlgorithmHandle, KeyHandle>(
          std::move(algorithm), std::move(symmetric_key)));
}

clonecore::Status validate_crypt_inputs(
    const std::size_t data_size,
    const std::size_t aad_size) {
  if (data_size > kTsumugiMaximumCryptBytes ||
      data_size > (std::numeric_limits<ULONG>::max)() ||
      aad_size > (std::numeric_limits<ULONG>::max)()) {
    return clonecore::Status::failure(crypto_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi暗号化範囲確認",
        L"暗号化対象または認証追加情報が固定上限を超えています"));
  }
  return clonecore::success_status();
}

template <std::size_t Size>
clonecore::Result<std::array<std::byte, Size>> random_bytes(
    const std::wstring_view operation) {
  std::array<std::byte, Size> result{};
  const NTSTATUS status = BCryptGenRandom(
      nullptr,
      reinterpret_cast<PUCHAR>(result.data()),
      static_cast<ULONG>(result.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<std::array<std::byte, Size>>::failure(
        crypto_error(
            clonecore::ErrorCode::internal_error,
            static_cast<DWORD>(status),
            std::wstring(operation),
            L"Windows CNGのシステム乱数を取得できませんでした"));
  }
  return clonecore::Result<std::array<std::byte, Size>>::success(result);
}

}  // namespace

TsumugiKey::TsumugiKey(
    std::array<std::byte, kTsumugiKeyBytes> bytes) noexcept
    : bytes_(bytes) {
  SecureZeroMemory(bytes.data(), bytes.size());
}

TsumugiKey::~TsumugiKey() { clear(); }

TsumugiKey::TsumugiKey(TsumugiKey&& other) noexcept
    : bytes_(other.bytes_) {
  other.clear();
}

TsumugiKey& TsumugiKey::operator=(TsumugiKey&& other) noexcept {
  if (this != &other) {
    clear();
    bytes_ = other.bytes_;
    other.clear();
  }
  return *this;
}

std::span<const std::byte, kTsumugiKeyBytes> TsumugiKey::bytes() const
    noexcept {
  return bytes_;
}

void TsumugiKey::clear() noexcept {
  SecureZeroMemory(bytes_.data(), bytes_.size());
}

TsumugiPasswordAssessment assess_tsumugi_password(
    const std::string_view password) noexcept {
  if (password.size() < 8U || password.size() > 1024U ||
      !std::all_of(password.begin(), password.end(), [](const char value) {
        const unsigned char byte = static_cast<unsigned char>(value);
        return byte >= 0x20U && byte <= 0x7eU;
      })) {
    return {};
  }
  bool has_lower = false;
  bool has_upper = false;
  bool has_digit = false;
  bool has_symbol = false;
  for (const char value : password) {
    has_lower = has_lower || (value >= 'a' && value <= 'z');
    has_upper = has_upper || (value >= 'A' && value <= 'Z');
    has_digit = has_digit || (value >= '0' && value <= '9');
    has_symbol = has_symbol ||
        !((value >= 'a' && value <= 'z') ||
          (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9'));
  }
  const int classes = static_cast<int>(has_lower) +
      static_cast<int>(has_upper) + static_cast<int>(has_digit) +
      static_cast<int>(has_symbol);
  return TsumugiPasswordAssessment{
      .accepted = true,
      .weak = password.size() < 12U || classes < 3,
  };
}

clonecore::Result<std::array<std::byte, kTsumugiSaltBytes>>
generate_tsumugi_salt() {
  return random_bytes<kTsumugiSaltBytes>(L"Tsumugi暗号Salt生成");
}

clonecore::Result<std::array<std::byte, kTsumugiGcmNonceBytes>>
generate_tsumugi_nonce() {
  return random_bytes<kTsumugiGcmNonceBytes>(L"Tsumugi暗号Nonce生成");
}

clonecore::Result<TsumugiKey> derive_tsumugi_key_argon2id(
    const std::string_view password,
    const TsumugiArgon2Parameters& parameters) {
  const auto assessment = assess_tsumugi_password(password);
  if (!assessment.accepted || parameters.memory_kib < 8U ||
      parameters.memory_kib > 1024U * 1024U ||
      parameters.iterations == 0U || parameters.iterations > 16U ||
      parameters.parallelism == 0U || parameters.parallelism > 16U) {
    return clonecore::Result<TsumugiKey>::failure(crypto_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Argon2id鍵導出設定",
        L"パスワードまたは記録済みArgon2idパラメーターが不正です"));
  }

  std::array<std::byte, kTsumugiKeyBytes> key{};
  const int status = argon2id_hash_raw(
      parameters.iterations,
      parameters.memory_kib,
      parameters.parallelism,
      password.data(),
      password.size(),
      parameters.salt.data(),
      parameters.salt.size(),
      key.data(),
      key.size());
  if (status != ARGON2_OK) {
    SecureZeroMemory(key.data(), key.size());
    return clonecore::Result<TsumugiKey>::failure(crypto_error(
        clonecore::ErrorCode::internal_error,
        static_cast<DWORD>(status),
        L"Argon2id鍵導出",
        L"Argon2id v1.3による暗号鍵導出に失敗しました"));
  }
  return clonecore::Result<TsumugiKey>::success(TsumugiKey(key));
}

clonecore::Result<TsumugiEncryptedBytes> encrypt_tsumugi_aes256_gcm(
    const TsumugiKey& key,
    const std::span<const std::byte, kTsumugiGcmNonceBytes> nonce,
    const std::span<const std::byte> additional_authenticated_data,
    const std::span<const std::byte> plaintext) {
  const auto valid = validate_crypt_inputs(
      plaintext.size(), additional_authenticated_data.size());
  if (!valid) {
    return clonecore::Result<TsumugiEncryptedBytes>::failure(valid.error());
  }
  auto opened = open_aes_key(key);
  if (!opened) {
    return clonecore::Result<TsumugiEncryptedBytes>::failure(opened.error());
  }
  auto handles = opened.take_value();
  TsumugiEncryptedBytes result{};
  result.ciphertext.resize(plaintext.size());

  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = reinterpret_cast<PUCHAR>(
      const_cast<std::byte*>(nonce.data()));
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbAuthData = reinterpret_cast<PUCHAR>(
      const_cast<std::byte*>(additional_authenticated_data.data()));
  info.cbAuthData =
      static_cast<ULONG>(additional_authenticated_data.size());
  info.pbTag = reinterpret_cast<PUCHAR>(result.tag.data());
  info.cbTag = static_cast<ULONG>(result.tag.size());

  ULONG written = 0;
  const NTSTATUS status = BCryptEncrypt(
      handles.second.get(),
      reinterpret_cast<PUCHAR>(
          const_cast<std::byte*>(plaintext.data())),
      static_cast<ULONG>(plaintext.size()),
      &info,
      nullptr,
      0,
      reinterpret_cast<PUCHAR>(result.ciphertext.data()),
      static_cast<ULONG>(result.ciphertext.size()),
      &written,
      0);
  if (!BCRYPT_SUCCESS(status) || written != result.ciphertext.size()) {
    if (!result.ciphertext.empty()) {
      SecureZeroMemory(result.ciphertext.data(), result.ciphertext.size());
    }
    return clonecore::Result<TsumugiEncryptedBytes>::failure(crypto_error(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(status),
        L"Tsumugi AES-256-GCM暗号化",
        L"暗号化または認証タグ生成に失敗しました"));
  }
  return clonecore::Result<TsumugiEncryptedBytes>::success(
      std::move(result));
}

clonecore::Result<std::vector<std::byte>> decrypt_tsumugi_aes256_gcm(
    const TsumugiKey& key,
    const std::span<const std::byte, kTsumugiGcmNonceBytes> nonce,
    const std::span<const std::byte> additional_authenticated_data,
    const std::span<const std::byte> ciphertext,
    const std::span<const std::byte, kTsumugiGcmTagBytes> tag) {
  const auto valid = validate_crypt_inputs(
      ciphertext.size(), additional_authenticated_data.size());
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }
  auto opened = open_aes_key(key);
  if (!opened) {
    return clonecore::Result<std::vector<std::byte>>::failure(opened.error());
  }
  auto handles = opened.take_value();
  std::vector<std::byte> plaintext(ciphertext.size());
  std::array<std::byte, kTsumugiGcmTagBytes> mutable_tag{};
  std::copy(tag.begin(), tag.end(), mutable_tag.begin());

  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = reinterpret_cast<PUCHAR>(
      const_cast<std::byte*>(nonce.data()));
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbAuthData = reinterpret_cast<PUCHAR>(
      const_cast<std::byte*>(additional_authenticated_data.data()));
  info.cbAuthData =
      static_cast<ULONG>(additional_authenticated_data.size());
  info.pbTag = reinterpret_cast<PUCHAR>(mutable_tag.data());
  info.cbTag = static_cast<ULONG>(mutable_tag.size());

  ULONG written = 0;
  const NTSTATUS status = BCryptDecrypt(
      handles.second.get(),
      reinterpret_cast<PUCHAR>(
          const_cast<std::byte*>(ciphertext.data())),
      static_cast<ULONG>(ciphertext.size()),
      &info,
      nullptr,
      0,
      reinterpret_cast<PUCHAR>(plaintext.data()),
      static_cast<ULONG>(plaintext.size()),
      &written,
      0);
  SecureZeroMemory(mutable_tag.data(), mutable_tag.size());
  if (!BCRYPT_SUCCESS(status) || written != plaintext.size()) {
    if (!plaintext.empty()) {
      SecureZeroMemory(plaintext.data(), plaintext.size());
    }
    return clonecore::Result<std::vector<std::byte>>::failure(crypto_error(
        clonecore::ErrorCode::verification_failed,
        static_cast<DWORD>(status),
        L"Tsumugi AES-256-GCM復号検証",
        L"パスワード、認証情報、または暗号化データが一致しません"));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(plaintext));
}

}  // namespace ytec::imageformat

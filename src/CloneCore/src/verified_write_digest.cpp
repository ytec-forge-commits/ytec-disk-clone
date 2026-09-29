#include "verified_write_digest.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::clonecore::detail {
namespace {

constexpr std::size_t kMaximumHashObjectBytes = 1024U * 1024U;
constexpr std::size_t kMaximumVerifiedWriteBytes = 16U * 1024U * 1024U;
constexpr std::string_view kVerifiedWriteDomain =
    "Y-TEC:Tsumugi:offline-clone:verified-write:v1";

Error digest_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

Error cng_error(const std::wstring_view operation, const NTSTATUS status) {
  return digest_error(
      ErrorCode::verification_failed,
      static_cast<DWORD>(status),
      std::wstring(operation),
      L"Windows CNGによる書込み検証SHA-256証跡の計算に失敗しました");
}

template <typename T>
std::array<std::byte, sizeof(T)> little_endian_bytes(const T value) noexcept {
  static_assert(std::is_unsigned_v<T>);
  std::array<std::byte, sizeof(T)> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(
        (value >> (index * 8U)) & static_cast<T>(0xFFU));
  }
  return bytes;
}

}  // namespace

struct VerifiedWriteDigestBuilder::State final {
  ~State() {
    if (hash != nullptr) {
      BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
    }
  }

  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;

  [[nodiscard]] Status update(const std::span<const std::byte> bytes) {
    if (finished || hash == nullptr) {
      return Status::failure(digest_error(
          ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"書込み検証SHA-256更新",
          L"完了済みまたは未初期化のSHA-256状態は更新できません"));
    }
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
      const ULONG amount = static_cast<ULONG>(std::min<std::size_t>(
          bytes.size() - consumed,
          std::numeric_limits<ULONG>::max()));
      const NTSTATUS status = BCryptHashData(
          hash,
          reinterpret_cast<PUCHAR>(
              const_cast<std::byte*>(bytes.data() + consumed)),
          amount,
          0);
      if (!BCRYPT_SUCCESS(status)) {
        return Status::failure(cng_error(L"書込み検証SHA-256更新", status));
      }
      consumed += amount;
    }
    return success_status();
  }

  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  std::vector<UCHAR> hash_object;
  std::uint64_t record_count{};
  std::uint64_t verified_payload_bytes{};
  bool finished{};
};

VerifiedWriteDigestBuilder::VerifiedWriteDigestBuilder(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

VerifiedWriteDigestBuilder::~VerifiedWriteDigestBuilder() = default;
VerifiedWriteDigestBuilder::VerifiedWriteDigestBuilder(
    VerifiedWriteDigestBuilder&&) noexcept = default;
VerifiedWriteDigestBuilder& VerifiedWriteDigestBuilder::operator=(
    VerifiedWriteDigestBuilder&&) noexcept = default;

Result<VerifiedWriteDigestBuilder> VerifiedWriteDigestBuilder::create() {
  auto state = std::make_unique<State>();
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &state->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    return Result<VerifiedWriteDigestBuilder>::failure(
        cng_error(L"書込み検証SHA-256初期化", status));
  }

  ULONG object_length = 0;
  ULONG returned = 0;
  status = BCryptGetProperty(
      state->algorithm,
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return Result<VerifiedWriteDigestBuilder>::failure(
        cng_error(L"書込み検証SHA-256オブジェクト長取得", status));
  }
  if (returned != sizeof(object_length) || object_length == 0U ||
      object_length > kMaximumHashObjectBytes) {
    return Result<VerifiedWriteDigestBuilder>::failure(digest_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"書込み検証SHA-256オブジェクト長検証",
        L"Windows CNGが安全な上限内のSHA-256状態サイズを返しませんでした"));
  }

  ULONG digest_length = 0;
  status = BCryptGetProperty(
      state->algorithm,
      BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&digest_length),
      sizeof(digest_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return Result<VerifiedWriteDigestBuilder>::failure(
        cng_error(L"書込み検証SHA-256出力長取得", status));
  }
  if (returned != sizeof(digest_length) ||
      digest_length != VerifiedWriteDigest{}.size()) {
    return Result<VerifiedWriteDigestBuilder>::failure(digest_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"書込み検証SHA-256出力長検証",
        L"Windows CNGが32バイト以外のSHA-256出力長を返しました"));
  }

  state->hash_object.resize(object_length);
  // This is an ordinary SHA-256 hash, not a keyed/HMAC construction.
  status = BCryptCreateHash(
      state->algorithm,
      &state->hash,
      state->hash_object.data(),
      object_length,
      nullptr,
      0,
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return Result<VerifiedWriteDigestBuilder>::failure(
        cng_error(L"書込み検証SHA-256ハッシュ生成", status));
  }
  return Result<VerifiedWriteDigestBuilder>::success(
      VerifiedWriteDigestBuilder(std::move(state)));
}

Status VerifiedWriteDigestBuilder::append_verified_write(
    const std::uint64_t offset,
    const std::span<const std::byte> read_back_bytes) {
  if (!state_ || state_->finished) {
    return Status::failure(digest_error(
        ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"書込み検証SHA-256記録",
        L"完了済みまたは無効なSHA-256証跡へ追記できません"));
  }
  if (read_back_bytes.empty() ||
      read_back_bytes.size() > kMaximumVerifiedWriteBytes) {
    return Status::failure(digest_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"書込み検証SHA-256記録長",
        L"検証済み書込み長が安全な対応範囲外です"));
  }
  const auto length = static_cast<std::uint64_t>(read_back_bytes.size());
  if (length > std::numeric_limits<std::uint64_t>::max() - offset ||
      state_->record_count == std::numeric_limits<std::uint64_t>::max() ||
      length > std::numeric_limits<std::uint64_t>::max() -
          state_->verified_payload_bytes) {
    return Status::failure(digest_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"書込み検証SHA-256境界",
        L"検証済み書込み範囲または累積長がオーバーフローしました"));
  }

  const auto domain = std::as_bytes(std::span(kVerifiedWriteDomain));
  const auto encoded_offset = little_endian_bytes(offset);
  const auto encoded_length = little_endian_bytes(length);
  for (const std::span<const std::byte> component : {
           domain,
           std::span<const std::byte>(encoded_offset),
           std::span<const std::byte>(encoded_length),
           read_back_bytes}) {
    const Status updated = state_->update(component);
    if (!updated) {
      return updated;
    }
  }
  ++state_->record_count;
  state_->verified_payload_bytes += length;
  return success_status();
}

Result<VerifiedWriteDigest> VerifiedWriteDigestBuilder::finish() {
  if (!state_ || state_->finished) {
    return Result<VerifiedWriteDigest>::failure(digest_error(
        ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"書込み検証SHA-256完了",
        L"SHA-256証跡は一度だけ完了できます"));
  }
  if (state_->record_count == 0U) {
    return Result<VerifiedWriteDigest>::failure(digest_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"書込み検証SHA-256完了",
        L"検証済み書込みがないためSHA-256証跡を確定できません"));
  }

  // Mark first so even an unexpected provider failure cannot permit a retry
  // against a possibly consumed hash state.
  state_->finished = true;
  VerifiedWriteDigest digest{};
  const NTSTATUS status = BCryptFinishHash(
      state_->hash,
      reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return Result<VerifiedWriteDigest>::failure(
        cng_error(L"書込み検証SHA-256完了", status));
  }
  return Result<VerifiedWriteDigest>::success(digest);
}

}  // namespace ytec::clonecore::detail

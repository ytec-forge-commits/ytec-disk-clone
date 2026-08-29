#include "ytec/winpeapp/resume_slot_admission.h"

#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::array<std::byte, 12U> kAdmissionHashDomain{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'C'},
    std::byte{'P'}, std::byte{'E'}, std::byte{'A'}, std::byte{'D'},
    std::byte{'M'}, std::byte{'I'}, std::byte{'T'}, std::byte{'1'},
};
constexpr std::size_t kMaximumAdmissionTextFields = 16U;
constexpr std::size_t kMaximumAdmissionDigests = 16U;
constexpr std::size_t kMaximumAdmissionFieldCharacters = 32U * 1024U;
constexpr std::size_t kMaximumAdmissionCanonicalBytes = 256U * 1024U;

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_utf16(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  append_u64(bytes, static_cast<std::uint64_t>(value.size()));
  for (const wchar_t character : value) {
    const auto code_unit = static_cast<std::uint16_t>(character);
    bytes.push_back(static_cast<std::byte>(code_unit & 0xFFU));
    bytes.push_back(static_cast<std::byte>((code_unit >> 8U) & 0xFFU));
  }
}

[[nodiscard]] clonecore::Error admission_error(
    std::wstring message,
    const clonecore::ErrorCode code = clonecore::ErrorCode::invalid_data,
    const DWORD native_code = ERROR_INVALID_DATA) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"WinPE SingleResumeSlot admission plan",
      .message = std::move(message),
  };
}

}  // namespace

clonecore::Result<operationcore::OperationPlan>
make_winpe_resume_slot_admission_plan(
    operationcore::OperationId operation_id,
    const operationcore::OperationKind kind,
    std::optional<clonecore::StableDiskIdentity> source,
    std::optional<clonecore::StableDiskIdentity> target,
    const std::uint64_t expected_work_bytes,
    const std::span<const std::wstring_view> immutable_review_fields,
    const std::span<const operationcore::Sha256Digest>
        immutable_review_digests) {
  if (immutable_review_fields.empty() && immutable_review_digests.empty()) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(
            L"実レビュー済みのoutput／操作固有条件が一つも拘束されていません"));
  }
  if (immutable_review_fields.size() > kMaximumAdmissionTextFields ||
      immutable_review_digests.size() > kMaximumAdmissionDigests) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビュー拘束フィールド数が安全上限を超えています"));
  }

  std::size_t canonical_size = kAdmissionHashDomain.size() + 1U + 4U + 4U;
  for (const auto field : immutable_review_fields) {
    if (field.empty()) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(L"空のレビュー拘束フィールドは使用できません"));
    }
    if (field.size() > kMaximumAdmissionFieldCharacters) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(
              L"レビュー拘束フィールドがUTF-16文字数上限を超えています"));
    }
    const std::size_t field_bytes = field.size() * sizeof(std::uint16_t);
    if (canonical_size > kMaximumAdmissionCanonicalBytes - 8U ||
        field_bytes >
            kMaximumAdmissionCanonicalBytes - (canonical_size + 8U)) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(L"レビュー拘束のcanonical byte上限を超えています"));
    }
    canonical_size += 8U + field_bytes;
  }
  constexpr std::size_t kDigestBytes =
      std::tuple_size_v<operationcore::Sha256Digest>;
  const std::size_t digest_bytes =
      immutable_review_digests.size() * kDigestBytes;
  if (digest_bytes > kMaximumAdmissionCanonicalBytes - canonical_size) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビューHash拘束のcanonical byte上限を超えています"));
  }
  canonical_size += digest_bytes;

  try {
    std::vector<std::byte> binding;
    binding.reserve(canonical_size);
    binding.insert(
        binding.end(),
        kAdmissionHashDomain.begin(),
        kAdmissionHashDomain.end());
    binding.push_back(static_cast<std::byte>(kind));
    append_u32(
        binding, static_cast<std::uint32_t>(immutable_review_fields.size()));
    for (const auto field : immutable_review_fields) {
      append_utf16(binding, field);
    }
    append_u32(
        binding, static_cast<std::uint32_t>(immutable_review_digests.size()));
    for (const auto& digest : immutable_review_digests) {
      binding.insert(binding.end(), digest.begin(), digest.end());
    }

    auto immutable_payload_hash = imageformat::sha256(binding);
    if (!immutable_payload_hash) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          immutable_payload_hash.error());
    }
    operationcore::OperationPlan plan{
        .operation_id = operation_id,
        .kind = kind,
        .environment = operationcore::OperationEnvironment::winpe,
        .source = std::move(source),
        .target = std::move(target),
        .expected_work_bytes = expected_work_bytes,
        .immutable_payload_hash = immutable_payload_hash.take_value(),
    };
    const auto valid = operationcore::validate_operation_plan(plan);
    if (!valid) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          valid.error());
    }
    return clonecore::Result<operationcore::OperationPlan>::success(
        std::move(plan));
  } catch (const std::bad_alloc&) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(
            L"レビュー拘束Hash用メモリを確保できません",
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY));
  } catch (const std::length_error&) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビュー拘束Hashの配列長が安全上限外です"));
  }
}

clonecore::Status guard_new_winpe_operation_start(
    const operationcore::OperationPlan& admission_plan,
    operationcore::IResumeSlotPlatform& platform) {
  operationcore::SingleResumeSlot slot(platform);
  return slot.guard_new_operation_start(admission_plan);
}

clonecore::Status guard_bound_winpe_restore_resume(
    const operationcore::ResumeSlotBinding& reviewed_binding,
    operationcore::IResumeSlotPlatform& platform) {
  operationcore::SingleResumeSlot slot(platform);
  const auto bound = slot.open_bound(reviewed_binding);
  if (!bound) {
    return clonecore::Status::failure(bound.error());
  }
  return clonecore::success_status();
}

}  // namespace ytec::winpeapp

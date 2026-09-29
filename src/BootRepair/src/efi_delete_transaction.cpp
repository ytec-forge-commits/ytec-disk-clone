#include "ytec/bootrepair/efi_delete_transaction.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::wstring_view kQuarantineNamespace =
    L"YTEC-EFI-REMOVE-QUARANTINE-V1";
constexpr std::wstring_view kEfiSystemPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::size_t kMaximumManifestMaterialBytes =
    16U * 1024U * 1024U;

clonecore::Error delete_error(
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

bool same_text(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t left_character, const wchar_t right_character) {
            return std::towlower(left_character) ==
                std::towlower(right_character);
          });
}

std::wstring folded_text(const std::wstring_view value) {
  std::wstring folded(value);
  std::transform(
      folded.begin(),
      folded.end(),
      folded.begin(),
      [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
      });
  return folded;
}

bool case_insensitive_less(
    const std::wstring_view left,
    const std::wstring_view right) {
  const std::wstring folded_left = folded_text(left);
  const std::wstring folded_right = folded_text(right);
  if (folded_left != folded_right) {
    return folded_left < folded_right;
  }
  return left < right;
}

bool is_ascii_hex(const wchar_t character) noexcept {
  return (character >= L'0' && character <= L'9') ||
      (character >= L'a' && character <= L'f') ||
      (character >= L'A' && character <= L'F');
}

bool is_guid_body(const std::wstring_view body) noexcept {
  if (body.size() != 36U) {
    return false;
  }
  for (std::size_t index = 0U; index < body.size(); ++index) {
    const bool hyphen_position =
        index == 8U || index == 13U || index == 18U || index == 23U;
    if (hyphen_position ? body[index] != L'-'
                        : !is_ascii_hex(body[index])) {
      return false;
    }
  }
  return true;
}

bool is_braced_guid(const std::wstring_view value) noexcept {
  return value.size() == 38U && value.front() == L'{' &&
      value.back() == L'}' && is_guid_body(value.substr(1U, 36U));
}

bool is_canonical_volume_guid_root(const std::wstring_view value) noexcept {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  return value.size() == 49U &&
      same_text(value.substr(0U, prefix.size()), prefix) &&
      is_guid_body(value.substr(prefix.size(), 36U)) &&
      value[value.size() - 2U] == L'}' && value.back() == L'\\';
}

bool is_reserved_dos_name(const std::wstring_view segment) {
  const std::size_t dot = segment.find(L'.');
  const std::wstring base = folded_text(segment.substr(0U, dot));
  if (base == L"con" || base == L"prn" || base == L"aux" ||
      base == L"nul" || base == L"clock$") {
    return true;
  }
  if (base.size() == 4U &&
      (base.substr(0U, 3U) == L"com" ||
       base.substr(0U, 3U) == L"lpt") &&
      base[3] >= L'1' && base[3] <= L'9') {
    return true;
  }
  return false;
}

bool safe_path_segment(const std::wstring_view segment) {
  if (segment.empty() || segment.size() > kMaximumEfiDeleteNameCharacters ||
      segment == L"." || segment == L".." ||
      segment.front() == L'.' || segment.front() == L' ' ||
      segment.back() == L'.' || segment.back() == L' ' ||
      is_reserved_dos_name(segment)) {
    return false;
  }
  constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
  return std::none_of(
      segment.begin(),
      segment.end(),
      [&](const wchar_t character) {
        const auto code = static_cast<std::uint32_t>(character);
        return code < 0x20U || code > 0x7EU ||
            forbidden.find(character) != std::wstring_view::npos;
      });
}

bool safe_relative_path(
    const std::wstring_view path,
    std::size_t* const segment_count) {
  if (path.empty() ||
      path.size() > kMaximumEfiDeleteRelativePathCharacters ||
      path.front() == L'\\' || path.back() == L'\\' ||
      path.find(L'/') != std::wstring_view::npos) {
    return false;
  }
  std::size_t count = 0U;
  std::size_t begin = 0U;
  while (begin < path.size()) {
    const std::size_t end = path.find(L'\\', begin);
    const std::wstring_view segment = path.substr(
        begin,
        end == std::wstring_view::npos ? path.size() - begin : end - begin);
    if (!safe_path_segment(segment)) {
      return false;
    }
    ++count;
    if (count > kMaximumEfiDeleteTreeDepth + 1U) {
      return false;
    }
    if (end == std::wstring_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  if (segment_count != nullptr) {
    *segment_count = count;
  }
  return count != 0U;
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(
      value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{0};
      });
}

bool safe_candidate_name(const std::wstring_view name) {
  return safe_path_segment(name) && !same_text(name, L"Microsoft") &&
      !same_text(name, L"Boot") &&
      !same_text(name, L"EFI") && !same_text(name, kQuarantineNamespace);
}

clonecore::Status validate_disk_and_esp(
    const EfiDeleteReviewObservation& observation) {
  const clonecore::Status disk = clonecore::validate_stable_identity(
      observation.disk, observation.disk, L"第三者EFI削除レビュー対象");
  if (!disk) {
    return disk;
  }
  const auto& esp = observation.esp;
  const std::uint64_t disk_size = observation.disk.size_bytes;
  const std::uint64_t sector_size = observation.disk.logical_sector_size;
  const bool geometry_overflow =
      esp.offset_bytes > (std::numeric_limits<std::uint64_t>::max)() -
          esp.length_bytes;
  if (esp.partition_number == 0U || esp.offset_bytes == 0U ||
      esp.length_bytes == 0U || esp.volume_serial_number == 0U ||
      !is_braced_guid(esp.partition_identifier) ||
      !is_braced_guid(esp.partition_type_identifier) ||
      !same_text(
          esp.partition_type_identifier, kEfiSystemPartitionType) ||
      !is_canonical_volume_guid_root(esp.volume_guid_root) ||
      !same_text(esp.filesystem_name, L"FAT32") ||
      geometry_overflow || esp.offset_bytes + esp.length_bytes > disk_size ||
      esp.offset_bytes % sector_size != 0U ||
      esp.length_bytes % sector_size != 0U) {
    return clonecore::Status::failure(delete_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"第三者EFI削除ESP安定識別",
        L"ESPのGUID、filesystem serial、位置、長さ、または境界が安全な値ではありません"));
  }
  return clonecore::success_status();
}

bool entry_equal(
    const EfiDeleteTreeEntryManifest& left,
    const EfiDeleteTreeEntryManifest& right) noexcept {
  return left.kind == right.kind &&
      left.relative_path == right.relative_path &&
      left.volume_serial_number == right.volume_serial_number &&
      left.file_id == right.file_id &&
      left.size_bytes == right.size_bytes &&
      left.creation_time == right.creation_time &&
      left.last_access_time == right.last_access_time &&
      left.last_write_time == right.last_write_time &&
      left.change_time == right.change_time &&
      left.hard_link_count == right.hard_link_count &&
      left.file_sha256 == right.file_sha256 &&
      left.has_file_sha256 == right.has_file_sha256;
}

bool candidate_equal(
    const EfiDeleteCandidateManifest& left,
    const EfiDeleteCandidateManifest& right) noexcept {
  return left.relative_name == right.relative_name &&
      left.entries.size() == right.entries.size() &&
      std::equal(
          left.entries.begin(),
          left.entries.end(),
          right.entries.begin(),
          entry_equal);
}

template <typename Integer>
bool append_little_endian(
    std::vector<std::byte>& output,
    const Integer value) {
  static_assert(std::is_integral_v<Integer>);
  using Unsigned = std::make_unsigned_t<Integer>;
  if (output.size() > kMaximumManifestMaterialBytes - sizeof(Integer)) {
    return false;
  }
  const Unsigned converted = static_cast<Unsigned>(value);
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    output.push_back(static_cast<std::byte>(
        (converted >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
  }
  return true;
}

bool append_bytes(
    std::vector<std::byte>& output,
    const std::span<const std::byte> value) {
  if (value.size() > kMaximumManifestMaterialBytes - output.size()) {
    return false;
  }
  output.insert(output.end(), value.begin(), value.end());
  return true;
}

bool append_wstring(
    std::vector<std::byte>& output,
    const std::wstring_view value) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)() ||
      !append_little_endian(
          output, static_cast<std::uint32_t>(value.size()))) {
    return false;
  }
  for (const wchar_t character : value) {
    if (!append_little_endian(
            output, static_cast<std::uint16_t>(character))) {
      return false;
    }
  }
  return true;
}

clonecore::Error hash_error(
    const std::wstring_view operation,
    const NTSTATUS status) {
  return delete_error(
      clonecore::ErrorCode::verification_failed,
      static_cast<DWORD>(status),
      std::wstring(operation),
      L"第三者EFI削除manifestのSHA-256処理に失敗しました");
}

class AlgorithmHandle final {
 public:
  AlgorithmHandle() = default;
  ~AlgorithmHandle() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0U);
    }
  }
  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

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

  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

clonecore::Result<EfiDeleteSha256> sha256_manifest_material(
    const std::span<const std::byte> material) {
  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        hash_error(L"第三者EFI削除SHA-256初期化", status));
  }

  ULONG object_length = 0U;
  ULONG returned = 0U;
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &returned,
      0U);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_length) ||
      object_length == 0U) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        hash_error(L"第三者EFI削除SHA-256作業長", status));
  }

  ULONG digest_length = 0U;
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&digest_length),
      sizeof(digest_length),
      &returned,
      0U);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(digest_length) ||
      digest_length != EfiDeleteSha256{}.size()) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        hash_error(L"第三者EFI削除SHA-256出力長", status));
  }

  std::vector<UCHAR> object(object_length);
  HashHandle hash;
  status = BCryptCreateHash(
      algorithm.get(),
      hash.put(),
      object.data(),
      object_length,
      nullptr,
      0U,
      0U);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        hash_error(L"第三者EFI削除SHA-256生成", status));
  }
  if (material.size() > (std::numeric_limits<ULONG>::max)()) {
    return clonecore::Result<EfiDeleteSha256>::failure(delete_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"第三者EFI削除manifest長",
        L"manifest材料がSHA-256処理上限を超えています"));
  }
  status = BCryptHashData(
      hash.get(),
      reinterpret_cast<PUCHAR>(
          const_cast<std::byte*>(material.data())),
      static_cast<ULONG>(material.size()),
      0U);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        hash_error(L"第三者EFI削除SHA-256入力", status));
  }
  EfiDeleteSha256 digest{};
  status = BCryptFinishHash(
      hash.get(),
      reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()),
      0U);
  if (!BCRYPT_SUCCESS(status)) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        hash_error(L"第三者EFI削除SHA-256完了", status));
  }
  return clonecore::Result<EfiDeleteSha256>::success(digest);
}

clonecore::Result<EfiDeleteSha256> make_manifest_hash(
    const std::vector<EfiDeleteCandidateManifest>& candidates) {
  std::vector<std::byte> material;
  constexpr std::string_view domain = "YTEC-EFI-REMOVE-MANIFEST-V1";
  material.reserve(4U * 1024U);
  if (!append_little_endian(
          material, static_cast<std::uint32_t>(domain.size())) ||
      !append_bytes(
          material,
          std::as_bytes(std::span(domain.data(), domain.size()))) ||
      !append_little_endian(
          material, static_cast<std::uint32_t>(candidates.size()))) {
    return clonecore::Result<EfiDeleteSha256>::failure(delete_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"第三者EFI削除manifest直列化",
        L"manifest材料が固定上限を超えています"));
  }
  for (const auto& candidate : candidates) {
    if (!append_wstring(material, candidate.relative_name) ||
        !append_little_endian(
            material,
            static_cast<std::uint32_t>(candidate.entries.size()))) {
      return clonecore::Result<EfiDeleteSha256>::failure(delete_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"第三者EFI削除candidate直列化",
          L"candidate manifestが固定上限を超えています"));
    }
    for (const auto& entry : candidate.entries) {
      const auto kind = static_cast<std::uint8_t>(entry.kind);
      const auto has_hash = static_cast<std::uint8_t>(
          entry.has_file_sha256 ? 1U : 0U);
      if (!append_little_endian(material, kind) ||
          !append_wstring(material, entry.relative_path) ||
          !append_little_endian(material, entry.volume_serial_number) ||
          !append_bytes(material, entry.file_id) ||
          !append_little_endian(material, entry.size_bytes) ||
          !append_little_endian(material, entry.creation_time) ||
          !append_little_endian(material, entry.last_access_time) ||
          !append_little_endian(material, entry.last_write_time) ||
          !append_little_endian(material, entry.change_time) ||
          !append_little_endian(material, entry.hard_link_count) ||
          !append_little_endian(material, has_hash) ||
          !append_bytes(material, entry.file_sha256)) {
        return clonecore::Result<EfiDeleteSha256>::failure(delete_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_BUFFER_OVERFLOW,
            L"第三者EFI削除entry直列化",
            L"tree entry manifestが固定上限を超えています"));
      }
    }
  }
  return sha256_manifest_material(material);
}

EfiDeletePlatformFailureKind failure_from_error(
    const clonecore::Error& error) noexcept {
  switch (error.code) {
    case clonecore::ErrorCode::identity_mismatch:
    case clonecore::ErrorCode::invalid_data:
    case clonecore::ErrorCode::unsupported_layout:
      return EfiDeletePlatformFailureKind::tamper_detected;
    case clonecore::ErrorCode::verification_failed:
      return EfiDeletePlatformFailureKind::verification_failure;
    default:
      return EfiDeletePlatformFailureKind::io_failure;
  }
}

bool valid_completed_step(const EfiDeletePlatformStepResult& step) noexcept {
  return step.succeeded &&
      step.failure_kind == EfiDeletePlatformFailureKind::none &&
      step.mutation_extent == EfiDeleteMutationExtent::complete &&
      !step.error.has_value();
}

bool valid_failed_step(const EfiDeletePlatformStepResult& step) noexcept {
  return !step.succeeded &&
      step.failure_kind != EfiDeletePlatformFailureKind::none &&
      step.mutation_extent != EfiDeleteMutationExtent::complete &&
      step.error.has_value();
}

bool valid_object_identity(
    const EfiDeleteObjectIdentity& identity,
    const std::uint64_t expected_volume_serial) noexcept {
  return identity.volume_serial_number == expected_volume_serial &&
      identity.volume_serial_number != 0U && !all_zero(identity.file_id);
}

clonecore::Error platform_contract_error(
    const std::wstring_view operation) {
  return delete_error(
      clonecore::ErrorCode::internal_error,
      ERROR_INVALID_STATE,
      std::wstring(operation),
      L"platform adapterがhandle-bound transaction契約と矛盾する結果を返しました");
}

struct RollbackSummary final {
  bool complete{true};
  bool contract_violation{};
  std::size_t restored{};
  EfiDeletePlatformFailureKind failure_kind{
      EfiDeletePlatformFailureKind::none};
  std::optional<clonecore::Error> error;
};

RollbackSummary rollback_quarantined_candidates(
    const ReviewedEfiDeletePlan& reviewed,
    const std::span<const std::size_t> moved_indices,
    const EfiDeleteObjectIdentity& quarantine_identity,
    const bool cleanup_allowed,
    IEfiDeleteTransactionPlatform& platform) {
  RollbackSummary summary;
  for (auto cursor = moved_indices.rbegin();
       cursor != moved_indices.rend(); ++cursor) {
    const std::size_t index = *cursor;
    const auto& candidate = reviewed.candidates()[index];
    const EfiDeletePlatformStepResult restored =
        platform.rollback_candidate_from_quarantine_handle_bound(
            reviewed, index, candidate, quarantine_identity);
    if (!valid_completed_step(restored)) {
      summary.complete = false;
      if (!valid_failed_step(restored)) {
        summary.contract_violation = true;
        if (!summary.error.has_value()) {
          summary.failure_kind =
              EfiDeletePlatformFailureKind::platform_contract_violation;
          summary.error = platform_contract_error(
              L"第三者EFI quarantine rollback契約");
        }
      } else if (!summary.error.has_value()) {
        summary.failure_kind = restored.failure_kind;
        summary.error = restored.error;
      }
      continue;
    }
    ++summary.restored;
  }

  if (cleanup_allowed && summary.complete &&
      summary.restored == moved_indices.size()) {
    const EfiDeletePlatformStepResult cleanup =
        platform.remove_owned_quarantine_if_empty_handle_bound(
            reviewed, quarantine_identity);
    if (!valid_completed_step(cleanup)) {
      summary.complete = false;
      if (!valid_failed_step(cleanup)) {
        summary.contract_violation = true;
        summary.failure_kind =
            EfiDeletePlatformFailureKind::platform_contract_violation;
        summary.error = platform_contract_error(
            L"第三者EFI rollback後quarantine削除契約");
      } else {
        summary.failure_kind = cleanup.failure_kind;
        summary.error = cleanup.error;
      }
    }
  }
  return summary;
}

void apply_rollback_summary(
    EfiDeleteTransactionReport& report,
    const RollbackSummary& rollback) {
  report.rolled_back_candidates = rollback.restored;
  if (rollback.error.has_value()) {
    report.rollback_error = rollback.error;
    report.rollback_platform_failure = rollback.failure_kind;
  }
}

}  // namespace

std::wstring_view efi_delete_quarantine_namespace() noexcept {
  return kQuarantineNamespace;
}

ReviewedEfiDeletePlan::ReviewedEfiDeletePlan(
    clonecore::StableDiskIdentity disk,
    EfiDeleteEspIdentity esp,
    EfiBootOwnershipEvidence ownership,
    std::vector<EfiDeleteCandidateManifest> candidates,
    const EfiDeleteSha256 manifest_sha256)
    : disk_(std::move(disk)),
      esp_(std::move(esp)),
      ownership_(std::move(ownership)),
      candidates_(std::move(candidates)),
      manifest_sha256_(manifest_sha256) {}

const clonecore::StableDiskIdentity&
ReviewedEfiDeletePlan::expected_disk() const noexcept {
  return disk_;
}

const EfiDeleteEspIdentity& ReviewedEfiDeletePlan::expected_esp()
    const noexcept {
  return esp_;
}

const EfiBootOwnershipEvidence&
ReviewedEfiDeletePlan::expected_ownership() const noexcept {
  return ownership_;
}

std::span<const EfiDeleteCandidateManifest>
ReviewedEfiDeletePlan::candidates() const noexcept {
  return candidates_;
}

const EfiDeleteSha256& ReviewedEfiDeletePlan::manifest_sha256()
    const noexcept {
  return manifest_sha256_;
}

clonecore::Result<ReviewedEfiDeletePlan> review_efi_delete_candidates(
    const EfiDeleteReviewObservation& observation) {
  const clonecore::Status identity = validate_disk_and_esp(observation);
  if (!identity) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(identity.error());
  }
  if (observation.candidates.empty() ||
      observation.candidates.size() > kMaximumEfiDeleteCandidates) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"第三者EFI削除candidate件数",
        L"削除レビューには1件以上64件以下のcandidateが必要です"));
  }
  if (!observation.bounded_top_level_enumeration_complete ||
      !efi_boot_ownership_allows_third_party_preserve(
          observation.ownership) ||
      observation.ownership.top_level_non_microsoft_namespace_count !=
          static_cast<std::uint32_t>(observation.candidates.size())) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"第三者EFI削除top-level完全性",
        L"EFI直下のbounded完全列挙と独立した通常directoryだけの"
        L"所有権診断が必要です"));
  }

  std::vector<EfiDeleteCandidateManifest> candidates;
  candidates.reserve(observation.candidates.size());
  std::set<std::wstring> candidate_names;
  std::set<std::pair<std::uint64_t, EfiDeleteFileId>> all_file_ids;
  std::size_t total_entries = 0U;
  std::uint64_t total_file_bytes = 0U;

  for (const auto& candidate : observation.candidates) {
    if (!safe_candidate_name(candidate.relative_name)) {
      return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_NAME,
          L"第三者EFI削除candidate名",
          L"EFI直下の安全な非Microsoft通常directory名だけを指定できます"));
    }
    if (!candidate_names.insert(folded_text(candidate.relative_name)).second) {
      return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"第三者EFI削除candidate重複",
          L"大文字小文字だけが異なるcandidateを複数指定できません"));
    }
    if (candidate.entries.empty() ||
        candidate.entries.size() > kMaximumEfiDeleteTreeEntries ||
        total_entries >
            kMaximumEfiDeleteTreeEntries - candidate.entries.size()) {
      return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"第三者EFI削除tree件数",
          L"candidate treeが固定列挙上限を超えています"));
    }
    total_entries += candidate.entries.size();

    EfiDeleteCandidateManifest manifest{
        .relative_name = candidate.relative_name,
    };
    manifest.entries.reserve(candidate.entries.size());
    for (const auto& entry : candidate.entries) {
      std::size_t depth = 0U;
      const std::wstring prefix = candidate.relative_name + L"\\";
      const bool under_candidate =
          entry.relative_path == candidate.relative_name ||
          (entry.relative_path.size() > prefix.size() &&
           entry.relative_path.starts_with(prefix));
      if (!safe_relative_path(entry.relative_path, &depth) ||
          !under_candidate || depth == 0U ||
          !same_text(
              entry.relative_path.substr(
                  0U, std::min(entry.relative_path.size(),
                               candidate.relative_name.size())),
              candidate.relative_name)) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_NAME,
            L"第三者EFI削除relative path",
            L"tree entryはcandidate配下の固定上限内の相対pathである必要があります"));
      }
      if (entry.kind != EfiDeleteEntryKind::regular_file &&
          entry.kind != EfiDeleteEntryKind::directory) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_REPARSE_TAG_INVALID,
            L"第三者EFI削除object type",
            L"reparseまたは未知objectを含むtreeは削除対象にできません"));
      }
      if (!entry.file_id_valid || all_zero(entry.file_id) ||
          entry.volume_serial_number != observation.esp.volume_serial_number ||
          entry.hard_link_count != 1U ||
          !all_file_ids.insert(
              {entry.volume_serial_number, entry.file_id}).second) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"第三者EFI削除FileId",
            L"全entryに同一ESPの一意なFileIdと単一hard-linkが必要です"));
      }
      const bool is_file = entry.kind == EfiDeleteEntryKind::regular_file;
      if (is_file != entry.file_sha256_valid ||
          (!is_file && !all_zero(entry.file_sha256))) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"第三者EFI削除file SHA-256",
            L"通常fileにはSHA-256が必須でdirectoryには指定できません"));
      }
      if (is_file) {
        if (entry.size_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - total_file_bytes) {
          return clonecore::Result<ReviewedEfiDeletePlan>::failure(
              delete_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_ARITHMETIC_OVERFLOW,
                  L"第三者EFI削除file size合計",
                  L"file size合計が表現上限を超えています"));
        }
        total_file_bytes += entry.size_bytes;
        if (total_file_bytes > observation.esp.length_bytes) {
          return clonecore::Result<ReviewedEfiDeletePlan>::failure(
              delete_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_DISK_FULL,
                  L"第三者EFI削除file size整合性",
                  L"treeのfile size合計がESP長を超えています"));
        }
      }
      manifest.entries.push_back(EfiDeleteTreeEntryManifest{
          .kind = entry.kind,
          .relative_path = entry.relative_path,
          .volume_serial_number = entry.volume_serial_number,
          .file_id = entry.file_id,
          .size_bytes = entry.size_bytes,
          .creation_time = entry.creation_time,
          .last_access_time = entry.last_access_time,
          .last_write_time = entry.last_write_time,
          .change_time = entry.change_time,
          .hard_link_count = entry.hard_link_count,
          .file_sha256 = entry.file_sha256,
          .has_file_sha256 = entry.file_sha256_valid,
      });
    }

    std::sort(
        manifest.entries.begin(),
        manifest.entries.end(),
        [](const auto& left, const auto& right) {
          return case_insensitive_less(
              left.relative_path, right.relative_path);
        });
    std::map<std::wstring, EfiDeleteEntryKind> path_kinds;
    for (const auto& entry : manifest.entries) {
      const std::wstring folded = folded_text(entry.relative_path);
      if (!path_kinds.emplace(folded, entry.kind).second) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_DUP_NAME,
            L"第三者EFI削除relative path重複",
            L"大文字小文字だけが異なるtree entryを複数指定できません"));
      }
    }
    const auto root = path_kinds.find(folded_text(candidate.relative_name));
    if (root == path_kinds.end() ||
        root->second != EfiDeleteEntryKind::directory ||
        std::none_of(
            manifest.entries.begin(),
            manifest.entries.end(),
            [&](const auto& entry) {
              return entry.relative_path == candidate.relative_name &&
                  entry.kind == EfiDeleteEntryKind::directory;
            })) {
      return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_DIRECTORY,
          L"第三者EFI削除candidate root",
          L"candidate自身を同じ表記の通常directory entryとして含める必要があります"));
    }
    for (const auto& entry : manifest.entries) {
      if (entry.relative_path == candidate.relative_name) {
        continue;
      }
      const std::size_t separator = entry.relative_path.rfind(L'\\');
      if (separator == std::wstring::npos) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_NAME,
            L"第三者EFI削除parent path",
            L"candidate root以外のentryにはparent directoryが必要です"));
      }
      const auto parent = path_kinds.find(
          folded_text(entry.relative_path.substr(0U, separator)));
      if (parent == path_kinds.end() ||
          parent->second != EfiDeleteEntryKind::directory) {
        return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_PATH_NOT_FOUND,
            L"第三者EFI削除parent identity",
            L"全entryの直接parentを同じmanifest内のdirectoryとして確認する必要があります"));
      }
    }
    candidates.push_back(std::move(manifest));
  }

  std::sort(
      candidates.begin(),
      candidates.end(),
      [](const auto& left, const auto& right) {
        return case_insensitive_less(left.relative_name, right.relative_name);
      });
  auto hash = make_manifest_hash(candidates);
  if (!hash) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(hash.error());
  }
  return clonecore::Result<ReviewedEfiDeletePlan>::success(
      ReviewedEfiDeletePlan(
          observation.disk,
          observation.esp,
          observation.ownership,
          std::move(candidates),
          hash.take_value()));
}

clonecore::Result<ReviewedEfiDeletePlan>
review_efi_delete_candidates_read_only(
    const clonecore::StableDiskIdentity& expected_disk,
    const EfiDeleteEspIdentity& expected_esp,
    IEfiDeleteReadOnlyInspector& inspector) {
  auto observation =
      inspector.inspect_candidates_read_only(expected_disk, expected_esp);
  if (!observation) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(
        observation.error());
  }
  const clonecore::Status disk = clonecore::validate_stable_identity(
      expected_disk,
      observation.value().disk,
      L"第三者EFI削除レビュー対象");
  if (!disk) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(disk.error());
  }
  if (!equivalent_efi_delete_esp_identity(
          expected_esp, observation.value().esp)) {
    return clonecore::Result<ReviewedEfiDeletePlan>::failure(delete_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"第三者EFI削除レビューESP再識別",
        L"選択時と読取り専用レビュー時のESP安定識別が一致しません"));
  }
  return review_efi_delete_candidates(observation.value());
}

bool equivalent_efi_delete_esp_identity(
    const EfiDeleteEspIdentity& left,
    const EfiDeleteEspIdentity& right) noexcept {
  return left.partition_number == right.partition_number &&
      left.offset_bytes == right.offset_bytes &&
      left.length_bytes == right.length_bytes &&
      same_text(left.partition_identifier, right.partition_identifier) &&
      same_text(
          left.partition_type_identifier,
          right.partition_type_identifier) &&
      left.partition_attributes == right.partition_attributes &&
      same_text(left.volume_guid_root, right.volume_guid_root) &&
      same_text(left.filesystem_name, right.filesystem_name) &&
      left.volume_serial_number == right.volume_serial_number;
}

bool equivalent_efi_delete_manifest(
    const ReviewedEfiDeletePlan& left,
    const ReviewedEfiDeletePlan& right) noexcept {
  const auto left_candidates = left.candidates();
  const auto right_candidates = right.candidates();
  return left.manifest_sha256() == right.manifest_sha256() &&
      equivalent_efi_boot_ownership(
          left.expected_ownership(), right.expected_ownership()) &&
      left_candidates.size() == right_candidates.size() &&
      std::equal(
          left_candidates.begin(),
          left_candidates.end(),
          right_candidates.begin(),
          candidate_equal);
}

std::wstring efi_delete_source_relative_path(
    const EfiDeleteCandidateManifest& candidate) {
  return L"EFI\\" + candidate.relative_name;
}

clonecore::Result<std::wstring> efi_delete_quarantine_slot_relative_path(
    const std::size_t candidate_index) {
  if (candidate_index >= kMaximumEfiDeleteCandidates) {
    return clonecore::Result<std::wstring>::failure(delete_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"第三者EFI quarantine slot",
        L"candidate indexが固定上限を超えています"));
  }
  std::wstring suffix = std::to_wstring(candidate_index);
  suffix.insert(0U, 4U - suffix.size(), L'0');
  return clonecore::Result<std::wstring>::success(
      std::wstring(kQuarantineNamespace) + L"\\C" + suffix);
}

EfiDeletePlatformStepResult EfiDeletePlatformStepResult::completed() {
  return EfiDeletePlatformStepResult{
      .succeeded = true,
      .failure_kind = EfiDeletePlatformFailureKind::none,
      .mutation_extent = EfiDeleteMutationExtent::complete,
      .error = std::nullopt,
  };
}

EfiDeletePlatformStepResult EfiDeletePlatformStepResult::failed(
    const EfiDeletePlatformFailureKind failure_kind,
    const EfiDeleteMutationExtent mutation_extent,
    clonecore::Error error) {
  return EfiDeletePlatformStepResult{
      .succeeded = false,
      .failure_kind = failure_kind,
      .mutation_extent = mutation_extent,
      .error = std::move(error),
  };
}

EfiDeleteTransactionReport execute_efi_delete_transaction(
    const ReviewedEfiDeletePlan& reviewed,
    const EfiDeleteConfirmation& confirmation,
    IEfiDeleteTransactionPlatform& platform) {
  EfiDeleteTransactionReport report;
  if (!confirmation.destructive_warning_acknowledged ||
      confirmation.typed_token != L"OK") {
    report.failure_stage = EfiDeleteFailureStage::confirmation;
    report.primary_error = delete_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"第三者EFI削除確認",
        L"危険確認と大文字OKの入力が揃っていないため開始しません");
    return report;
  }

  auto fresh_observation =
      platform.inspect_candidates_read_only(
          reviewed.expected_disk(), reviewed.expected_esp());
  if (!fresh_observation) {
    report.failure_stage = EfiDeleteFailureStage::fresh_inspection;
    report.platform_failure = failure_from_error(fresh_observation.error());
    report.primary_error = fresh_observation.error();
    return report;
  }
  const clonecore::Status stable_disk = clonecore::validate_stable_identity(
      reviewed.expected_disk(),
      fresh_observation.value().disk,
      L"第三者EFI削除実行対象");
  if (!stable_disk || !equivalent_efi_delete_esp_identity(
          reviewed.expected_esp(), fresh_observation.value().esp)) {
    report.failure_stage = EfiDeleteFailureStage::target_identity;
    report.platform_failure = EfiDeletePlatformFailureKind::tamper_detected;
    report.primary_error = stable_disk
        ? delete_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"第三者EFI削除ESP再識別",
              L"レビュー時と実行直前のESP安定識別が一致しません")
        : stable_disk.error();
    return report;
  }
  report.stable_target_reidentified = true;

  auto fresh_plan = review_efi_delete_candidates(fresh_observation.value());
  if (!fresh_plan ||
      !equivalent_efi_delete_manifest(reviewed, fresh_plan.value())) {
    report.failure_stage = EfiDeleteFailureStage::tree_manifest;
    report.platform_failure = EfiDeletePlatformFailureKind::tamper_detected;
    report.primary_error = fresh_plan
        ? delete_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"第三者EFI削除tree exact再照合",
              L"candidate、relative path、type、FileId、size、times、"
              L"またはSHA-256がレビュー時から変わりました")
        : fresh_plan.error();
    return report;
  }
  report.fresh_manifest_verified = true;

  const EfiDeleteQuarantineCreateResult quarantine =
      platform.create_owned_quarantine_no_replace(reviewed);
  if (!valid_completed_step(quarantine.step) ||
      !valid_object_identity(
          quarantine.identity,
          reviewed.expected_esp().volume_serial_number)) {
    report.failure_stage = EfiDeleteFailureStage::quarantine_prepare;
    if (valid_failed_step(quarantine.step)) {
      report.platform_failure = quarantine.step.failure_kind;
      report.primary_error = quarantine.step.error;
      report.outcome = quarantine.step.mutation_extent ==
              EfiDeleteMutationExtent::none
          ? EfiDeleteTransactionOutcome::stopped_before_mutation
          : EfiDeleteTransactionOutcome::partial_rollback;
    } else {
      report.failure_stage = EfiDeleteFailureStage::platform_contract;
      report.platform_failure =
          EfiDeletePlatformFailureKind::platform_contract_violation;
      report.primary_error = platform_contract_error(
          L"第三者EFI quarantine作成契約");
      report.outcome = EfiDeleteTransactionOutcome::partial_rollback;
    }
    return report;
  }

  std::vector<std::size_t> moved_indices;
  moved_indices.reserve(reviewed.candidates().size());
  for (std::size_t index = 0U;
       index < reviewed.candidates().size(); ++index) {
    const EfiDeletePlatformStepResult moved =
        platform.move_candidate_to_quarantine_handle_bound(
            reviewed,
            index,
            reviewed.candidates()[index],
            quarantine.identity);
    if (!valid_completed_step(moved)) {
      report.failure_stage = EfiDeleteFailureStage::quarantine_move;
      const bool valid_failure = valid_failed_step(moved);
      report.platform_failure = valid_failure
          ? moved.failure_kind
          : EfiDeletePlatformFailureKind::platform_contract_violation;
      report.primary_error = valid_failure
          ? moved.error
          : std::optional<clonecore::Error>(platform_contract_error(
                L"第三者EFI quarantine移動契約"));
      const bool uncertain_current = !valid_failure ||
          moved.mutation_extent ==
              EfiDeleteMutationExtent::partial_or_unknown;
      const RollbackSummary rollback = rollback_quarantined_candidates(
          reviewed,
          moved_indices,
          quarantine.identity,
          !uncertain_current,
          platform);
      apply_rollback_summary(report, rollback);
      report.outcome = !uncertain_current && rollback.complete
          ? EfiDeleteTransactionOutcome::rolled_back
          : EfiDeleteTransactionOutcome::partial_rollback;
      if (rollback.contract_violation) {
        report.failure_stage = EfiDeleteFailureStage::rollback;
      }
      return report;
    }
    moved_indices.push_back(index);
    report.quarantined_candidates = moved_indices.size();
  }
  report.all_candidates_were_quarantined_before_bcd = true;

  const EfiDeletePlatformStepResult rebuilt =
      platform.rebuild_microsoft_bcd_and_verify_readback(reviewed);
  if (!valid_completed_step(rebuilt)) {
    report.failure_stage = EfiDeleteFailureStage::microsoft_bcd_rebuild;
    const bool valid_failure = valid_failed_step(rebuilt);
    report.platform_failure = valid_failure
        ? rebuilt.failure_kind
        : EfiDeletePlatformFailureKind::platform_contract_violation;
    report.primary_error = valid_failure
        ? rebuilt.error
        : std::optional<clonecore::Error>(platform_contract_error(
              L"第三者EFI削除BCDBoot transaction契約"));
    report.microsoft_bcd_failure_rollback_verified = valid_failure &&
        rebuilt.mutation_extent == EfiDeleteMutationExtent::none;
    const RollbackSummary rollback = rollback_quarantined_candidates(
        reviewed,
        moved_indices,
        quarantine.identity,
        true,
        platform);
    apply_rollback_summary(report, rollback);
    report.outcome = report.microsoft_bcd_failure_rollback_verified &&
            rollback.complete
        ? EfiDeleteTransactionOutcome::rolled_back
        : EfiDeleteTransactionOutcome::partial_rollback;
    if (rollback.contract_violation) {
      report.failure_stage = EfiDeleteFailureStage::rollback;
    }
    return report;
  }
  report.microsoft_bcd_rebuild_readback_verified = true;

  for (std::size_t index = 0U;
       index < reviewed.candidates().size(); ++index) {
    const EfiDeletePlatformStepResult removed =
        platform.delete_quarantined_candidate_tree_handle_bound(
            reviewed,
            index,
            reviewed.candidates()[index],
            quarantine.identity);
    if (!valid_completed_step(removed)) {
      report.failure_stage = EfiDeleteFailureStage::final_delete;
      const bool valid_failure = valid_failed_step(removed);
      report.platform_failure = valid_failure
          ? removed.failure_kind
          : EfiDeletePlatformFailureKind::platform_contract_violation;
      report.primary_error = valid_failure
          ? removed.error
          : std::optional<clonecore::Error>(platform_contract_error(
                L"第三者EFI handle-bound recursive deletion契約"));
      const bool current_unchanged = valid_failure &&
          removed.mutation_extent == EfiDeleteMutationExtent::none;
      std::vector<std::size_t> intact_indices;
      const std::size_t first_intact = current_unchanged
          ? index
          : index + 1U;
      intact_indices.reserve(reviewed.candidates().size() - first_intact);
      for (std::size_t intact = first_intact;
           intact < reviewed.candidates().size(); ++intact) {
        intact_indices.push_back(intact);
      }
      const RollbackSummary rollback = rollback_quarantined_candidates(
          reviewed,
          intact_indices,
          quarantine.identity,
          current_unchanged,
          platform);
      apply_rollback_summary(report, rollback);
      const bool candidates_restored_exactly =
          rollback.restored == intact_indices.size();
      if (report.deleted_candidates != 0U || !current_unchanged) {
        report.outcome = EfiDeleteTransactionOutcome::partial_delete;
      } else if (candidates_restored_exactly) {
        const EfiDeletePlatformStepResult bcd_rollback =
            platform.rollback_microsoft_bcd_rebuild_if_identity_matches(
                reviewed);
        if (valid_completed_step(bcd_rollback)) {
          report.microsoft_bcd_rolled_back_after_delete_stop = true;
          report.outcome = rollback.complete
              ? EfiDeleteTransactionOutcome::rolled_back
              : EfiDeleteTransactionOutcome::partial_rollback;
        } else {
          report.failure_stage = EfiDeleteFailureStage::rollback;
          const bool valid_bcd_failure =
              valid_failed_step(bcd_rollback);
          report.rollback_platform_failure = valid_bcd_failure
              ? bcd_rollback.failure_kind
              : EfiDeletePlatformFailureKind::
                    platform_contract_violation;
          report.rollback_error = valid_bcd_failure
              ? bcd_rollback.error
              : std::optional<clonecore::Error>(
                    platform_contract_error(
                        L"第三者EFI削除後BCD rollback契約"));
          report.outcome = EfiDeleteTransactionOutcome::partial_rollback;
        }
      } else {
        report.outcome = EfiDeleteTransactionOutcome::partial_rollback;
      }
      return report;
    }
    ++report.deleted_candidates;
  }

  const EfiDeletePlatformStepResult bcd_commit =
      platform.commit_microsoft_bcd_rebuild(reviewed);
  if (!valid_completed_step(bcd_commit)) {
    report.failure_stage = EfiDeleteFailureStage::microsoft_bcd_commit;
    const bool valid_failure = valid_failed_step(bcd_commit);
    report.platform_failure = valid_failure
        ? bcd_commit.failure_kind
        : EfiDeletePlatformFailureKind::platform_contract_violation;
    report.primary_error = valid_failure
        ? bcd_commit.error
        : std::optional<clonecore::Error>(platform_contract_error(
              L"第三者EFI削除BCD commit契約"));
    report.outcome =
        EfiDeleteTransactionOutcome::committed_bcd_cleanup_incomplete;
    return report;
  }
  report.microsoft_bcd_rollback_boundary_committed = true;

  const EfiDeletePlatformStepResult cleanup =
      platform.remove_owned_quarantine_if_empty_handle_bound(
          reviewed, quarantine.identity);
  if (!valid_completed_step(cleanup)) {
    report.failure_stage = EfiDeleteFailureStage::quarantine_cleanup;
    const bool valid_failure = valid_failed_step(cleanup);
    report.platform_failure = valid_failure
        ? cleanup.failure_kind
        : EfiDeletePlatformFailureKind::platform_contract_violation;
    report.primary_error = valid_failure
        ? cleanup.error
        : std::optional<clonecore::Error>(platform_contract_error(
              L"第三者EFI最終quarantine削除契約"));
    report.outcome =
        EfiDeleteTransactionOutcome::committed_quarantine_cleanup_incomplete;
    return report;
  }

  report.outcome = EfiDeleteTransactionOutcome::committed;
  report.failure_stage = EfiDeleteFailureStage::none;
  report.platform_failure = EfiDeletePlatformFailureKind::none;
  return report;
}

}  // namespace ytec::bootrepair

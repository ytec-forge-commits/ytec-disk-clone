#include "ytec/operationcore/windows_resume_slot_platform.h"

#include "sha256_internal.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::operationcore {
namespace {

constexpr std::uint16_t kResumeSlotMajor = 1U;
constexpr std::uint16_t kResumeSlotMinorV1 = 0U;
constexpr std::uint16_t kResumeSlotMinorV2 = 1U;
constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::array<std::byte, 8U> kResumeSlotMagic{
    std::byte{0x59}, std::byte{0x54}, std::byte{0x45}, std::byte{0x43},
    std::byte{0x52}, std::byte{0x53}, std::byte{0x31}, std::byte{0x00}};
constexpr std::string_view kPartialIdentityDomain =
    "YTEC-RESUME-PARTIAL-FILE-ID-V1";
constexpr std::string_view kOwnedObjectIdentityDomain =
    "YTEC-RESUME-OWNED-OBJECT-FILE-ID-V1";

clonecore::Error platform_error(
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

clonecore::Status platform_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(platform_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <typename T>
clonecore::Result<T> result_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(platform_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size() ||
      left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool operation_id_equal(
    const OperationId& left,
    const OperationId& right) noexcept {
  unsigned int difference = 0U;
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

bool identities_equal(
    const ResumeIdentityBinding& left,
    const ResumeIdentityBinding& right) noexcept {
  return detail::digest_equal(
             left.source_identity_hash, right.source_identity_hash) &&
         detail::digest_equal(
             left.target_identity_hash, right.target_identity_hash) &&
         detail::digest_equal(
             left.output_identity_hash, right.output_identity_hash);
}

bool partial_bindings_equal(
    const ResumeOwnedPartialBinding& left,
    const ResumeOwnedPartialBinding& right) noexcept {
  return operation_id_equal(left.operation_id, right.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.file_object_identity_hash,
             right.file_object_identity_hash);
}

bool optional_partial_bindings_equal(
    const std::optional<ResumeOwnedPartialBinding>& left,
    const std::optional<ResumeOwnedPartialBinding>& right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left || partial_bindings_equal(*left, *right);
}

bool owned_object_binding_equal(
    const ResumeOwnedObjectBinding& left,
    const ResumeOwnedObjectBinding& right) noexcept {
  return left.role == right.role &&
      operation_id_equal(left.operation_id, right.operation_id) &&
      identities_equal(left.identities, right.identities) &&
      detail::digest_equal(
          left.file_object_identity_hash,
          right.file_object_identity_hash);
}

bool owned_object_bindings_equal(
    const std::vector<ResumeOwnedObjectBinding>& left,
    const std::vector<ResumeOwnedObjectBinding>& right) noexcept {
  return left.size() == right.size() && std::equal(
      left.begin(),
      left.end(),
      right.begin(),
      owned_object_binding_equal);
}

bool owned_object_review_bindings_equal(
    const std::vector<ResumeOwnedObjectReviewBinding>& left,
    const std::vector<ResumeOwnedObjectReviewBinding>& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (left[index].role != right[index].role ||
        !detail::digest_equal(
            left[index].file_object_identity_hash,
            right[index].file_object_identity_hash)) {
      return false;
    }
  }
  return true;
}

bool records_equal(
    const ResumeSlotRecord& left,
    const ResumeSlotRecord& right) noexcept {
  return left.capability == right.capability &&
         operation_id_equal(
             left.checkpoint.checkpoint.operation_id,
             right.checkpoint.checkpoint.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.checkpoint.record_hash,
             right.checkpoint.record_hash) &&
         optional_partial_bindings_equal(
             left.owned_partial, right.owned_partial) &&
         owned_object_bindings_equal(
             left.owned_objects, right.owned_objects);
}

bool binding_equal(
    const ResumeSlotBinding& left,
    const ResumeSlotBinding& right) noexcept {
  if (left.capability != right.capability ||
      !operation_id_equal(left.operation_id, right.operation_id) ||
      !identities_equal(left.identities, right.identities) ||
      !detail::digest_equal(
          left.checkpoint_record_hash,
          right.checkpoint_record_hash) ||
      left.partial_file_object_identity_hash.has_value() !=
          right.partial_file_object_identity_hash.has_value() ||
      !owned_object_review_bindings_equal(
          left.owned_object_file_bindings,
          right.owned_object_file_bindings)) {
    return false;
  }
  return !left.partial_file_object_identity_hash ||
         detail::digest_equal(
             *left.partial_file_object_identity_hash,
             *right.partial_file_object_identity_hash);
}

bool known_capability(const std::uint8_t value) noexcept {
  switch (static_cast<ResumeCapability>(value)) {
    case ResumeCapability::persistent_exact_restore:
    case ResumeCapability::persistent_rescue_restore:
    case ResumeCapability::persistent_pe_exact_image_create:
    case ResumeCapability::same_process_only_vss_image_create:
    case ResumeCapability::same_process_only_vss_clone:
    case ResumeCapability::same_process_only_pe_image_create:
    case ResumeCapability::same_process_only_pe_clone:
    case ResumeCapability::unsupported_shrink_migration:
    case ResumeCapability::unsupported_raw_rescue:
      return true;
  }
  return false;
}

bool known_owned_object_role(const ResumeOwnedObjectRole role) noexcept {
  switch (role) {
    case ResumeOwnedObjectRole::image_partial:
    case ResumeOwnedObjectRole::image_resume_journal:
    case ResumeOwnedObjectRole::rescue_stage:
      return true;
  }
  return false;
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& path,
    const std::wstring_view operation) {
  if (path.size() < 3U || path.size() >= kMaximumPathCharacters ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'/') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      path.find(L'\0') != std::wstring::npos ||
      path.ends_with(L"\\") || path.ends_with(L" ") ||
      path.ends_with(L".")) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"正規化済みのローカル絶対パスが必要です");
  }

  std::size_t component_begin = 3U;
  while (component_begin < path.size()) {
    const std::size_t separator = path.find(L'\\', component_begin);
    const std::size_t component_end =
        separator == std::wstring::npos ? path.size() : separator;
    const std::wstring_view component(path.data() + component_begin,
                                      component_end - component_begin);
    if (component.empty() || component == L"." || component == L".." ||
        component.ends_with(L" ") || component.ends_with(L".")) {
      return result_failure<std::wstring>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_BAD_PATHNAME,
          std::wstring(operation),
          L"空要素、相対要素、末尾空白または末尾dotを含むパスは使用できません");
    }
    if (separator == std::wstring::npos) {
      break;
    }
    component_begin = separator + 1U;
  }

  std::vector<wchar_t> resolved(kMaximumPathCharacters, L'\0');
  wchar_t* file_part{};
  const DWORD length = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(resolved.size()),
      resolved.data(),
      &file_part);
  if (length == 0U || length >= resolved.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  const std::wstring canonical(resolved.data(), length);
  if (!equals_ordinal_ignore_case(path, canonical)) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"相対要素または正規化差分を含むパスは使用できません");
  }
  return clonecore::Result<std::wstring>::success(canonical);
}

clonecore::Result<std::wstring> parent_path(
    const std::wstring& path,
    const std::wstring_view operation) {
  const std::size_t separator = path.find_last_of(L'\\');
  if (separator == std::wstring::npos || separator < 2U) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"安全な親ディレクトリを導出できません");
  }
  if (separator == 2U) {
    return clonecore::Result<std::wstring>::success(path.substr(0U, 3U));
  }
  return clonecore::Result<std::wstring>::success(path.substr(0U, separator));
}

clonecore::Result<std::wstring> child_path(
    const std::wstring& parent,
    const std::wstring_view child,
    const std::wstring_view operation) {
  const bool parent_is_root = parent.ends_with(L"\\");
  const std::size_t separator_size = parent_is_root ? 0U : 1U;
  if (parent.size() + separator_size + child.size() >=
      kMaximumPathCharacters) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        std::wstring(operation),
        L"導出パスがWindows上限を超えます");
  }
  return clonecore::Result<std::wstring>::success(
      parent + (parent_is_root ? L"" : L"\\") + std::wstring(child));
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

clonecore::Status verify_opened_path(
    const HANDLE handle,
    const std::wstring& expected,
    const std::wstring_view operation) {
  std::vector<wchar_t> actual(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle,
      actual.data(),
      static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0U || length >= actual.size()) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::identity_mismatch,
        std::wstring(operation),
        length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  if (!equals_ordinal_ignore_case(
          std::wstring_view(actual.data(), length),
          extended_path(expected))) {
    return platform_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"opened handleの実体パスが固定パスと一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status verify_regular_directory(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle directory(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        std::wstring(operation),
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          directory.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        std::wstring(operation),
        GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return platform_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparseディレクトリではありません");
  }
  return verify_opened_path(directory.get(), path, operation);
}

clonecore::Status verify_directory_chain(
    const std::wstring& directory,
    const std::wstring_view operation) {
  std::size_t separator = directory.find(L'\\', 3U);
  while (separator != std::wstring::npos) {
    const auto status = verify_regular_directory(
        directory.substr(0U, separator), operation);
    if (!status) {
      return status;
    }
    separator = directory.find(L'\\', separator + 1U);
  }
  return verify_regular_directory(directory, operation);
}

clonecore::Status verify_regular_executable(
    const std::wstring& path) {
  clonecore::UniqueHandle executable(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!executable) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"Resume Slot EXE配置確認",
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          executable.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"Resume Slot EXE属性確認",
        GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
    return platform_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"Resume Slot EXE配置確認",
        L"通常の非reparseファイルではありません");
  }
  return verify_opened_path(
      executable.get(), path, L"Resume Slot EXE実体パス確認");
}

struct FileObservation final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};
  std::uint64_t file_size{};
  std::uint64_t allocation_size{};
  std::uint32_t link_count{};
  LARGE_INTEGER last_write{};
  LARGE_INTEGER change_time{};
};

clonecore::Result<FileObservation> observe_regular_single_link_file(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_ID_INFO identifier{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          handle, FileIdInfo, &identifier, sizeof(identifier)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic))) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      standard.EndOfFile.QuadPart < 0 ||
      standard.AllocationSize.QuadPart < 0 ||
      standard.NumberOfLinks != 1U) {
    return result_failure<FileObservation>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparse単一linkファイルとして識別できません");
  }
  FileObservation result{
      .volume_serial = identifier.VolumeSerialNumber,
      .file_size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .allocation_size =
          static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
      .link_count = standard.NumberOfLinks,
      .last_write = basic.LastWriteTime,
      .change_time = basic.ChangeTime,
  };
  static_assert(sizeof(identifier.FileId.Identifier) == 16U);
  std::memcpy(
      result.file_id.data(),
      identifier.FileId.Identifier,
      result.file_id.size());
  return clonecore::Result<FileObservation>::success(result);
}

bool same_file_object(
    const FileObservation& left,
    const FileObservation& right) noexcept {
  return left.volume_serial == right.volume_serial &&
         left.file_id == right.file_id;
}

bool same_complete_observation(
    const FileObservation& left,
    const FileObservation& right) noexcept {
  return same_file_object(left, right) &&
         left.file_size == right.file_size &&
         left.allocation_size == right.allocation_size &&
         left.link_count == right.link_count &&
         left.last_write.QuadPart == right.last_write.QuadPart &&
         left.change_time.QuadPart == right.change_time.QuadPart;
}

// A same-volume rename is expected to advance ChangeTime while preserving the
// file object and every content-bearing observation.  This comparison is
// deliberately used only across the no-replace publish rename.
bool same_content_observation_across_rename(
    const FileObservation& left,
    const FileObservation& right) noexcept {
  return same_file_object(left, right) &&
         left.file_size == right.file_size &&
         left.allocation_size == right.allocation_size &&
         left.link_count == right.link_count &&
         left.last_write.QuadPart == right.last_write.QuadPart;
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xffU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void set_u32(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes[offset + (shift / 8U)] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void append_wstring(
    std::vector<std::byte>& bytes,
    const std::wstring& value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    append_u16(bytes, static_cast<std::uint16_t>(character));
  }
}

class Reader final {
 public:
  explicit Reader(const std::span<const std::byte> bytes) noexcept
      : bytes_(bytes) {}

  [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept {
    if (!has(1U)) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
    return true;
  }

  [[nodiscard]] bool read_u16(std::uint16_t& value) noexcept {
    if (!has(2U)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes_[offset_]) |
        (std::to_integer<std::uint16_t>(bytes_[offset_ + 1U]) << 8U));
    offset_ += 2U;
    return true;
  }

  [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
    if (!has(4U)) {
      return false;
    }
    value = 0U;
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
      value |= std::to_integer<std::uint32_t>(
                   bytes_[offset_ + (shift / 8U)])
               << shift;
    }
    offset_ += 4U;
    return true;
  }

  template <std::size_t Size>
  [[nodiscard]] bool read_array(
      std::array<std::byte, Size>& value) noexcept {
    if (!has(Size)) {
      return false;
    }
    std::copy_n(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
        Size,
        value.begin());
    offset_ += Size;
    return true;
  }

  [[nodiscard]] bool read_span(
      const std::size_t size,
      std::span<const std::byte>& value) noexcept {
    if (!has(size)) {
      return false;
    }
    value = bytes_.subspan(offset_, size);
    offset_ += size;
    return true;
  }

  [[nodiscard]] bool read_wstring(
      std::wstring& value,
      const std::size_t maximum_characters) {
    std::uint32_t count{};
    if (!read_u32(count) || count == 0U ||
        count > maximum_characters ||
        static_cast<std::size_t>(count) >
            ((std::numeric_limits<std::size_t>::max)() / 2U) ||
        !has(static_cast<std::size_t>(count) * 2U)) {
      return false;
    }
    value.clear();
    value.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
      std::uint16_t character{};
      if (!read_u16(character) || character == 0U) {
        return false;
      }
      value.push_back(static_cast<wchar_t>(character));
    }
    return true;
  }

  [[nodiscard]] bool at_end() const noexcept {
    return offset_ == bytes_.size();
  }

 private:
  [[nodiscard]] bool has(const std::size_t amount) const noexcept {
    return offset_ <= bytes_.size() && amount <= bytes_.size() - offset_;
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

clonecore::Result<Sha256Digest> partial_identity_hash(
    const FileObservation& observation) {
  std::vector<std::byte> bytes;
  bytes.reserve(kPartialIdentityDomain.size() + sizeof(std::uint64_t) +
                observation.file_id.size());
  for (const char character : kPartialIdentityDomain) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  append_u64(bytes, observation.volume_serial);
  append_array(bytes, observation.file_id);
  return detail::sha256(bytes);
}

clonecore::Result<Sha256Digest> owned_object_identity_hash(
    const ResumeOwnedObjectRole role,
    const FileObservation& observation) {
  std::vector<std::byte> bytes;
  bytes.reserve(kOwnedObjectIdentityDomain.size() + 1U +
                sizeof(std::uint64_t) + observation.file_id.size());
  for (const char character : kOwnedObjectIdentityDomain) {
    bytes.push_back(static_cast<std::byte>(character));
  }
  append_u8(bytes, static_cast<std::uint8_t>(role));
  append_u64(bytes, observation.volume_serial);
  append_array(bytes, observation.file_id);
  return detail::sha256(bytes);
}

struct StoredSlot final {
  ResumeSlotRecord record;
  std::optional<std::wstring> partial_path;
  std::vector<std::wstring> owned_object_paths;
  Sha256Digest envelope_hash{};
};

clonecore::Result<StoredSlot> parse_slot_envelope(
    const std::span<const std::byte> bytes) {
  constexpr std::size_t kHeaderBytes = 16U;
  constexpr std::size_t kMinimumPayloadBytes =
      1U + 1U + 2U + (3U * Sha256Digest{}.size()) + 4U;
  constexpr std::size_t kMinimumBytes =
      kHeaderBytes + kMinimumPayloadBytes + Sha256Digest{}.size();
  if (bytes.size() < kMinimumBytes ||
      bytes.size() > kMaximumWindowsResumeSlotBytes) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        bytes.size() > kMaximumWindowsResumeSlotBytes
            ? ERROR_FILE_TOO_LARGE
            : ERROR_INVALID_DATA,
        L"Resume Slot envelope寸法",
        L"slot寸法が有界形式の範囲外です");
  }

  const std::span<const std::byte> authenticated =
      bytes.first(bytes.size() - Sha256Digest{}.size());
  Sha256Digest stored_hash{};
  std::copy_n(
      bytes.end() - static_cast<std::ptrdiff_t>(stored_hash.size()),
      stored_hash.size(),
      stored_hash.begin());
  const auto calculated_hash = detail::sha256(authenticated);
  if (!calculated_hash ||
      !detail::digest_equal(calculated_hash.value(), stored_hash)) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Resume Slot envelope Hash",
        L"slot全体のSHA-256が一致しません");
  }

  Reader reader(authenticated);
  std::array<std::byte, kResumeSlotMagic.size()> magic{};
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint32_t total_size{};
  std::uint8_t capability{};
  std::uint8_t flags{};
  std::uint16_t reserved{};
  ResumeIdentityBinding identities{};
  std::uint32_t checkpoint_size{};
  std::span<const std::byte> checkpoint_bytes;
  if (!reader.read_array(magic) || magic != kResumeSlotMagic ||
      !reader.read_u16(major) || !reader.read_u16(minor) ||
      !reader.read_u32(total_size) || major != kResumeSlotMajor ||
      (minor != kResumeSlotMinorV1 && minor != kResumeSlotMinorV2) ||
      total_size != bytes.size() ||
      !reader.read_u8(capability) || !known_capability(capability) ||
      !reader.read_u8(flags) ||
      (flags & ~(minor == kResumeSlotMinorV1 ? 0x01U : 0x02U)) != 0U ||
      !reader.read_u16(reserved) || reserved != 0U ||
      !reader.read_array(identities.source_identity_hash) ||
      !reader.read_array(identities.target_identity_hash) ||
      !reader.read_array(identities.output_identity_hash) ||
      !reader.read_u32(checkpoint_size) || checkpoint_size == 0U ||
      checkpoint_size > kMaximumCheckpointBytes ||
      !reader.read_span(checkpoint_size, checkpoint_bytes)) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope構造",
        L"版、固定フィールド、flagsまたはcheckpoint寸法が不正です");
  }

  auto checkpoint = parse_checkpoint(checkpoint_bytes);
  if (!checkpoint) {
    return clonecore::Result<StoredSlot>::failure(checkpoint.error());
  }
  ResumeSlotRecord record{
      .capability = static_cast<ResumeCapability>(capability),
      .checkpoint = checkpoint.take_value(),
      .identities = identities,
      .owned_partial = std::nullopt,
      .owned_objects = {},
  };
  std::optional<std::wstring> partial_path_value;
  std::vector<std::wstring> owned_object_paths;
  if ((flags & 0x01U) != 0U) {
    ResumeOwnedPartialBinding partial{};
    std::wstring partial_path;
    if (!reader.read_array(partial.operation_id) ||
        !reader.read_array(partial.identities.source_identity_hash) ||
        !reader.read_array(partial.identities.target_identity_hash) ||
        !reader.read_array(partial.identities.output_identity_hash) ||
        !reader.read_array(partial.file_object_identity_hash) ||
        !reader.read_wstring(partial_path, kMaximumPathCharacters - 1U)) {
      return result_failure<StoredSlot>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot owned partial構造",
          L"owned partialのbindingまたはpathが不正です");
    }
    auto canonical = canonical_local_path(
        partial_path, L"Resume Slot owned partial path");
    if (!canonical ||
        canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return canonical
          ? result_failure<StoredSlot>(
                clonecore::ErrorCode::invalid_data,
                ERROR_BAD_PATHNAME,
                L"Resume Slot owned partial path",
                L"所有対象は.partial拡張子でなければなりません")
          : clonecore::Result<StoredSlot>::failure(canonical.error());
    }
    record.owned_partial = partial;
    partial_path_value = canonical.take_value();
  }
  if ((flags & 0x02U) != 0U) {
    std::uint8_t object_count{};
    if (!reader.read_u8(object_count) || object_count == 0U ||
        object_count > kMaximumResumeOwnedObjects) {
      return result_failure<StoredSlot>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot owned object件数",
          L"owned object件数が有界範囲外です");
    }
    record.owned_objects.reserve(object_count);
    owned_object_paths.reserve(object_count);
    for (std::uint8_t index = 0U; index < object_count; ++index) {
      std::uint8_t role{};
      std::uint8_t object_reserved8{};
      std::uint16_t object_reserved16{};
      ResumeOwnedObjectBinding object{};
      std::wstring object_path;
      if (!reader.read_u8(role) || !reader.read_u8(object_reserved8) ||
          !reader.read_u16(object_reserved16) || object_reserved8 != 0U ||
          object_reserved16 != 0U ||
          !reader.read_array(object.operation_id) ||
          !reader.read_array(object.identities.source_identity_hash) ||
          !reader.read_array(object.identities.target_identity_hash) ||
          !reader.read_array(object.identities.output_identity_hash) ||
          !reader.read_array(object.file_object_identity_hash) ||
          !reader.read_wstring(
              object_path, kMaximumPathCharacters - 1U)) {
        return result_failure<StoredSlot>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Resume Slot owned object構造",
            L"owned objectのrole、binding、またはpathが不正です");
      }
      object.role = static_cast<ResumeOwnedObjectRole>(role);
      auto canonical = canonical_local_path(
          object_path, L"Resume Slot owned object path");
      if (!canonical || canonical.value().size() < 8U ||
          !equals_ordinal_ignore_case(
              std::wstring_view(canonical.value()).substr(
                  canonical.value().size() - 8U),
              L".partial")) {
        return canonical
            ? result_failure<StoredSlot>(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_BAD_PATHNAME,
                  L"Resume Slot owned object path",
                  L"所有対象は.partial拡張子でなければなりません")
            : clonecore::Result<StoredSlot>::failure(canonical.error());
      }
      for (const auto& existing_path : owned_object_paths) {
        if (equals_ordinal_ignore_case(existing_path, canonical.value())) {
          return result_failure<StoredSlot>(
              clonecore::ErrorCode::invalid_data,
              ERROR_DUP_NAME,
              L"Resume Slot owned object path",
              L"同じpathを複数のowned object roleに束縛できません");
        }
      }
      record.owned_objects.push_back(object);
      owned_object_paths.push_back(canonical.take_value());
    }
  }
  if ((minor == kResumeSlotMinorV1 && !owned_object_paths.empty()) ||
      (minor == kResumeSlotMinorV2 && owned_object_paths.empty())) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        ERROR_REVISION_MISMATCH,
        L"Resume Slot owned object版",
        L"envelope版とowned object束縛が一致しません");
  }
  if (!reader.at_end()) {
    return result_failure<StoredSlot>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope終端",
        L"未解釈または余分なフィールドがあります");
  }
  const auto valid = validate_resume_slot_record(record);
  if (!valid) {
    return clonecore::Result<StoredSlot>::failure(valid.error());
  }
  return clonecore::Result<StoredSlot>::success(StoredSlot{
      .record = std::move(record),
      .partial_path = std::move(partial_path_value),
      .owned_object_paths = std::move(owned_object_paths),
      .envelope_hash = stored_hash,
  });
}

clonecore::Result<std::vector<std::byte>> serialize_slot_envelope(
    const ResumeSlotRecord& record,
    const std::optional<std::wstring>& partial_path,
    const std::vector<std::wstring>& owned_object_paths) {
  const auto valid = validate_resume_slot_record(record);
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }
  if (record.owned_partial.has_value() != partial_path.has_value()) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope生成",
        L"owned partial bindingとpathの有無が一致しません");
  }
  if (record.owned_objects.size() != owned_object_paths.size() ||
      (!record.owned_objects.empty() && record.owned_partial)) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Resume Slot envelope生成",
        L"owned object bindingとpathの件数が一致しません");
  }
  std::optional<std::wstring> canonical_partial;
  if (partial_path) {
    auto canonical = canonical_local_path(
        *partial_path, L"Resume Slot owned partial path");
    if (!canonical || canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return canonical
          ? result_failure<std::vector<std::byte>>(
                clonecore::ErrorCode::invalid_argument,
                ERROR_BAD_PATHNAME,
                L"Resume Slot owned partial path",
                L"所有対象は.partial拡張子でなければなりません")
          : clonecore::Result<std::vector<std::byte>>::failure(
                canonical.error());
    }
    canonical_partial = canonical.take_value();
  }
  std::vector<std::wstring> canonical_objects;
  canonical_objects.reserve(owned_object_paths.size());
  for (const auto& path : owned_object_paths) {
    auto canonical = canonical_local_path(
        path, L"Resume Slot owned object path");
    if (!canonical || canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return canonical
          ? result_failure<std::vector<std::byte>>(
                clonecore::ErrorCode::invalid_argument,
                ERROR_BAD_PATHNAME,
                L"Resume Slot owned object path",
                L"所有対象は.partial拡張子でなければなりません")
          : clonecore::Result<std::vector<std::byte>>::failure(
                canonical.error());
    }
    for (const auto& existing : canonical_objects) {
      if (equals_ordinal_ignore_case(existing, canonical.value())) {
        return result_failure<std::vector<std::byte>>(
            clonecore::ErrorCode::invalid_argument,
            ERROR_DUP_NAME,
            L"Resume Slot owned object path",
            L"同じpathを複数roleに使用できません");
      }
    }
    canonical_objects.push_back(canonical.take_value());
  }

  auto checkpoint = serialize_checkpoint(record.checkpoint.checkpoint);
  if (!checkpoint) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        checkpoint.error());
  }
  std::vector<std::byte> bytes;
  std::size_t path_characters{};
  for (const auto& path : canonical_objects) {
    path_characters += path.size();
  }
  bytes.reserve(16U + 100U + checkpoint.value().size() +
                (canonical_partial ? canonical_partial->size() * 2U + 152U
                                   : 0U) +
                canonical_objects.size() * 160U +
                path_characters * 2U + Sha256Digest{}.size());
  append_array(bytes, kResumeSlotMagic);
  append_u16(bytes, kResumeSlotMajor);
  append_u16(
      bytes,
      record.owned_objects.empty()
          ? kResumeSlotMinorV1
          : kResumeSlotMinorV2);
  constexpr std::size_t kTotalSizeOffset = 12U;
  append_u32(bytes, 0U);
  append_u8(bytes, static_cast<std::uint8_t>(record.capability));
  append_u8(
      bytes,
      static_cast<std::uint8_t>(
          (record.owned_partial ? 0x01U : 0U) |
          (!record.owned_objects.empty() ? 0x02U : 0U)));
  append_u16(bytes, 0U);
  append_array(bytes, record.identities.source_identity_hash);
  append_array(bytes, record.identities.target_identity_hash);
  append_array(bytes, record.identities.output_identity_hash);
  append_u32(
      bytes, static_cast<std::uint32_t>(checkpoint.value().size()));
  bytes.insert(
      bytes.end(), checkpoint.value().begin(), checkpoint.value().end());
  if (record.owned_partial) {
    append_array(bytes, record.owned_partial->operation_id);
    append_array(bytes, record.owned_partial->identities.source_identity_hash);
    append_array(bytes, record.owned_partial->identities.target_identity_hash);
    append_array(bytes, record.owned_partial->identities.output_identity_hash);
    append_array(bytes, record.owned_partial->file_object_identity_hash);
    append_wstring(bytes, *canonical_partial);
  }
  if (!record.owned_objects.empty()) {
    append_u8(bytes, static_cast<std::uint8_t>(record.owned_objects.size()));
    for (std::size_t index = 0U; index < record.owned_objects.size(); ++index) {
      const auto& object = record.owned_objects[index];
      append_u8(bytes, static_cast<std::uint8_t>(object.role));
      append_u8(bytes, 0U);
      append_u16(bytes, 0U);
      append_array(bytes, object.operation_id);
      append_array(bytes, object.identities.source_identity_hash);
      append_array(bytes, object.identities.target_identity_hash);
      append_array(bytes, object.identities.output_identity_hash);
      append_array(bytes, object.file_object_identity_hash);
      append_wstring(bytes, canonical_objects[index]);
    }
  }
  if (bytes.size() >
      kMaximumWindowsResumeSlotBytes - Sha256Digest{}.size()) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"Resume Slot envelope生成",
        L"slot内容が安全上限を超えます");
  }
  const std::size_t total_size = bytes.size() + Sha256Digest{}.size();
  set_u32(bytes, kTotalSizeOffset, static_cast<std::uint32_t>(total_size));
  const auto hash = detail::sha256(bytes);
  if (!hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(hash.error());
  }
  append_array(bytes, hash.value());
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Status seek_begin(
    const HANDLE handle,
    const std::wstring_view operation) {
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_bounded_file(
    const HANDLE handle,
    const std::size_t maximum_bytes,
    const std::wstring_view operation) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            GetLastError()));
  }
  if (size.QuadPart <= 0 ||
      static_cast<unsigned long long>(size.QuadPart) > maximum_bytes) {
    return result_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        size.QuadPart > 0 ? ERROR_FILE_TOO_LARGE : ERROR_INVALID_DATA,
        std::wstring(operation),
        L"ファイル寸法が安全上限外です");
  }
  const auto positioned = seek_begin(handle, operation);
  if (!positioned) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        positioned.error());
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>((std::min)(
        bytes.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    if (!ReadFile(
            handle,
            bytes.data() + consumed,
            amount,
            &read,
            nullptr) ||
        read == 0U) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              std::wstring(operation),
              native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code));
    }
    consumed += read;
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

struct ReadSlot final {
  StoredSlot stored;
  FileObservation file;
};

clonecore::Result<std::optional<ReadSlot>> read_slot_file(
    const std::wstring& path) {
  clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<std::optional<ReadSlot>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Resume Slot checkpointを開く",
            native_code));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"Resume Slot checkpoint属性確認");
  if (!before) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        before.error());
  }
  const auto path_matches = verify_opened_path(
      file.get(), path, L"Resume Slot checkpoint実体パス確認");
  if (!path_matches) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        path_matches.error());
  }
  auto bytes = read_bounded_file(
      file.get(),
      kMaximumWindowsResumeSlotBytes,
      L"Resume Slot checkpoint有界読取り");
  if (!bytes) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(bytes.error());
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"Resume Slot checkpoint読取り後属性確認");
  if (!after || !same_complete_observation(before.value(), after.value())) {
    return after
        ? result_failure<std::optional<ReadSlot>>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot checkpoint読取り後再識別",
              L"File ID、寸法、link数または時刻が読取り中に変化しました")
        : clonecore::Result<std::optional<ReadSlot>>::failure(after.error());
  }
  auto stored = parse_slot_envelope(bytes.value());
  if (!stored) {
    return clonecore::Result<std::optional<ReadSlot>>::failure(
        stored.error());
  }
  return clonecore::Result<std::optional<ReadSlot>>::success(
      std::optional<ReadSlot>(ReadSlot{
          .stored = stored.take_value(),
          .file = after.value(),
      }));
}

struct PartialObservation final {
  WindowsResumeOwnedPartial partial;
  FileObservation file;
};

clonecore::Result<PartialObservation> observe_owned_partial(
    const std::wstring& canonical_path,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities) {
  auto parent = parent_path(
      canonical_path, L"Resume Slot owned partial親path");
  if (!parent) {
    return clonecore::Result<PartialObservation>::failure(parent.error());
  }
  const auto parents = verify_directory_chain(
      parent.value(), L"Resume Slot owned partial親chain確認");
  if (!parents) {
    return clonecore::Result<PartialObservation>::failure(parents.error());
  }

  clonecore::UniqueHandle file(CreateFileW(
      extended_path(canonical_path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file) {
    return clonecore::Result<PartialObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::identity_mismatch,
            L"Resume Slot owned partialを開く",
            GetLastError()));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"Resume Slot owned partial属性確認");
  if (!before) {
    return clonecore::Result<PartialObservation>::failure(before.error());
  }
  const auto actual_path = verify_opened_path(
      file.get(), canonical_path, L"Resume Slot owned partial実体パス確認");
  if (!actual_path) {
    return clonecore::Result<PartialObservation>::failure(actual_path.error());
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"Resume Slot owned partial再識別");
  if (!after || !same_complete_observation(before.value(), after.value())) {
    return after
        ? result_failure<PartialObservation>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot owned partial再識別",
              L"File ID、寸法、link数または時刻が観測中に変化しました")
        : clonecore::Result<PartialObservation>::failure(after.error());
  }
  auto hash = partial_identity_hash(after.value());
  if (!hash) {
    return clonecore::Result<PartialObservation>::failure(hash.error());
  }
  const auto parents_after = verify_directory_chain(
      parent.value(), L"Resume Slot owned partial親chain再確認");
  if (!parents_after) {
    return clonecore::Result<PartialObservation>::failure(
        parents_after.error());
  }
  return clonecore::Result<PartialObservation>::success(PartialObservation{
      .partial = {
          .canonical_path = canonical_path,
          .binding = {
              .operation_id = operation_id,
              .identities = identities,
              .file_object_identity_hash = hash.value(),
          },
      },
      .file = after.value(),
  });
}

struct OwnedObjectObservation final {
  WindowsResumeOwnedObject object;
  FileObservation file;
};

clonecore::Result<OwnedObjectObservation> observe_owned_object(
    const std::wstring& canonical_path,
    const ResumeOwnedObjectRole role,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities) {
  auto parent = parent_path(
      canonical_path, L"Resume Slot owned object親path");
  if (!parent) {
    return clonecore::Result<OwnedObjectObservation>::failure(parent.error());
  }
  const auto parents = verify_directory_chain(
      parent.value(), L"Resume Slot owned object親chain確認");
  if (!parents) {
    return clonecore::Result<OwnedObjectObservation>::failure(parents.error());
  }

  clonecore::UniqueHandle file(CreateFileW(
      extended_path(canonical_path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file) {
    return clonecore::Result<OwnedObjectObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::identity_mismatch,
            L"Resume Slot owned objectを開く",
            GetLastError()));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"Resume Slot owned object属性確認");
  if (!before) {
    return clonecore::Result<OwnedObjectObservation>::failure(before.error());
  }
  const auto actual_path = verify_opened_path(
      file.get(), canonical_path, L"Resume Slot owned object実体path確認");
  if (!actual_path) {
    return clonecore::Result<OwnedObjectObservation>::failure(
        actual_path.error());
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"Resume Slot owned object再識別");
  if (!after || !same_complete_observation(before.value(), after.value())) {
    return after
        ? result_failure<OwnedObjectObservation>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot owned object再識別",
              L"File ID、寸法、link数または時刻が観測中に変化しました")
        : clonecore::Result<OwnedObjectObservation>::failure(after.error());
  }
  auto hash = owned_object_identity_hash(role, after.value());
  if (!hash) {
    return clonecore::Result<OwnedObjectObservation>::failure(hash.error());
  }
  const auto parents_after = verify_directory_chain(
      parent.value(), L"Resume Slot owned object親chain再確認");
  if (!parents_after) {
    return clonecore::Result<OwnedObjectObservation>::failure(
        parents_after.error());
  }
  return clonecore::Result<OwnedObjectObservation>::success({
      .object = {
          .canonical_path = canonical_path,
          .binding = {
              .role = role,
              .operation_id = operation_id,
              .identities = identities,
              .file_object_identity_hash = hash.value(),
          },
      },
      .file = after.value(),
  });
}

void set_delete_pending(const HANDLE handle, const bool pending) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = pending ? TRUE : FALSE;
  if (!SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition))) {
    throw GetLastError();
  }
}

clonecore::Status mark_delete_pending(
    const HANDLE handle,
    const bool pending,
    const std::wstring_view operation) {
  try {
    set_delete_pending(handle, pending);
    return clonecore::success_status();
  } catch (const DWORD native_code) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        native_code));
  }
}

struct StagedSlot final {
  FileObservation file;
  Sha256Digest envelope_hash{};
};

clonecore::Result<StagedSlot> write_verified_stage(
    const std::wstring& stage_path,
    const std::span<const std::byte> bytes,
    const ResumeSlotRecord& expected_record,
    const std::optional<std::wstring>& expected_partial_path,
    const std::vector<std::wstring>& expected_owned_object_paths) {
  clonecore::UniqueHandle stage(CreateFileW(
      extended_path(stage_path).c_str(),
      GENERIC_READ | GENERIC_WRITE | DELETE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!stage) {
    return clonecore::Result<StagedSlot>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Resume Slot stage CREATE_NEW",
            GetLastError()));
  }
  const auto fail_owned = [&stage](const clonecore::Error& error) {
    static_cast<void>(mark_delete_pending(
        stage.get(), true, L"Resume Slot stage失敗後破棄"));
    stage.reset();
    return clonecore::Result<StagedSlot>::failure(error);
  };
  const auto opened_path = verify_opened_path(
      stage.get(), stage_path, L"Resume Slot stage実体パス確認");
  if (!opened_path) {
    return fail_owned(opened_path.error());
  }
  auto initial = observe_regular_single_link_file(
      stage.get(), L"Resume Slot stage初期属性確認");
  if (!initial) {
    return fail_owned(initial.error());
  }

  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>((std::min)(
        bytes.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written{};
    if (!WriteFile(
            stage.get(),
            bytes.data() + consumed,
            amount,
            &written,
            nullptr) ||
        written == 0U) {
      const DWORD native_code = GetLastError();
      return fail_owned(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Resume Slot stage書込み",
          native_code == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native_code));
    }
    consumed += written;
  }
  if (!FlushFileBuffers(stage.get())) {
    return fail_owned(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Resume Slot stage flush",
        GetLastError()));
  }
  auto readback = read_bounded_file(
      stage.get(),
      kMaximumWindowsResumeSlotBytes,
      L"Resume Slot stage同一handle読戻し");
  if (!readback) {
    return fail_owned(readback.error());
  }
  auto parsed = parse_slot_envelope(readback.value());
  if (!parsed || !records_equal(parsed.value().record, expected_record) ||
      parsed.value().partial_path != expected_partial_path ||
      parsed.value().owned_object_paths != expected_owned_object_paths) {
    return fail_owned(parsed
        ? platform_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"Resume Slot stage読戻し",
              L"読戻したrecordまたはowned partial pathが一致しません")
        : parsed.error());
  }
  auto final = observe_regular_single_link_file(
      stage.get(), L"Resume Slot stage書込み後属性確認");
  if (!final || !same_file_object(initial.value(), final.value())) {
    return fail_owned(final
        ? platform_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot stage書込み後再識別",
              L"CREATE_NEWしたstageのFile IDが変化しました")
        : final.error());
  }
  const StagedSlot result{
      .file = final.value(),
      .envelope_hash = parsed.value().envelope_hash,
  };
  stage.reset();
  return clonecore::Result<StagedSlot>::success(result);
}

void discard_exact_stage(
    const std::wstring& stage_path,
    const StagedSlot& expected) noexcept {
  try {
    clonecore::UniqueHandle stage(CreateFileW(
        extended_path(stage_path).c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!stage) {
      return;
    }
    auto observed = observe_regular_single_link_file(
        stage.get(), L"Resume Slot exact stage cleanup属性");
    if (!observed || !same_complete_observation(expected.file, observed.value())) {
      return;
    }
    auto bytes = read_bounded_file(
        stage.get(),
        kMaximumWindowsResumeSlotBytes,
        L"Resume Slot exact stage cleanup読取り");
    if (!bytes) {
      return;
    }
    auto parsed = parse_slot_envelope(bytes.value());
    if (!parsed ||
        !detail::digest_equal(
            parsed.value().envelope_hash, expected.envelope_hash)) {
      return;
    }
    static_cast<void>(mark_delete_pending(
        stage.get(), true, L"Resume Slot exact stage cleanup"));
  } catch (...) {
  }
}

struct ConfiguredPaths final {
  std::wstring executable;
  std::wstring application_directory;
  std::wstring data_directory;
  std::wstring checkpoint;
  std::wstring stage;
};

clonecore::Result<ConfiguredPaths> configure_paths(
    const std::wstring& executable_path) {
  auto executable = canonical_local_path(
      executable_path, L"Resume Slot EXE path");
  if (!executable) {
    return clonecore::Result<ConfiguredPaths>::failure(executable.error());
  }
  auto application = parent_path(
      executable.value(), L"Resume Slot application path");
  if (!application) {
    return clonecore::Result<ConfiguredPaths>::failure(application.error());
  }
  auto data = child_path(
      application.value(), L"data", L"Resume Slot data path");
  if (!data) {
    return clonecore::Result<ConfiguredPaths>::failure(data.error());
  }
  auto checkpoint = child_path(
      data.value(), kResumeSlotFileName, L"Resume Slot checkpoint path");
  if (!checkpoint) {
    return clonecore::Result<ConfiguredPaths>::failure(checkpoint.error());
  }
  auto stage = child_path(
      data.value(), L"active.checkpoint.new", L"Resume Slot stage path");
  if (!stage) {
    return clonecore::Result<ConfiguredPaths>::failure(stage.error());
  }
  const auto application_safe = verify_directory_chain(
      application.value(), L"Resume Slot application parent chain");
  if (!application_safe) {
    return clonecore::Result<ConfiguredPaths>::failure(
        application_safe.error());
  }
  const auto executable_safe = verify_regular_executable(executable.value());
  if (!executable_safe) {
    return clonecore::Result<ConfiguredPaths>::failure(executable_safe.error());
  }
  const auto data_safe = verify_directory_chain(
      data.value(), L"Resume Slot EXE隣data chain");
  if (!data_safe) {
    return clonecore::Result<ConfiguredPaths>::failure(data_safe.error());
  }
  return clonecore::Result<ConfiguredPaths>::success(ConfiguredPaths{
      .executable = executable.take_value(),
      .application_directory = application.take_value(),
      .data_directory = data.take_value(),
      .checkpoint = checkpoint.take_value(),
      .stage = stage.take_value(),
  });
}

struct PlatformState final {
  ResumeSlotObservation observation;
  std::optional<ReadSlot> checkpoint;
  std::optional<PartialObservation> partial;
  std::optional<std::wstring> partial_path;
  std::vector<OwnedObjectObservation> owned_objects;
  std::vector<std::wstring> owned_object_paths;
};

struct PersistentPeImageCreatePlatformState final {
  PersistentPeExactImageCreateObservation public_observation;
  std::optional<ReadSlot> checkpoint;
  std::optional<OwnedObjectObservation> image;
  std::optional<OwnedObjectObservation> journal;
  std::wstring image_path;
  std::wstring journal_path;
};

bool file_not_found_error(const clonecore::Error& error) noexcept {
  return error.native_code == ERROR_FILE_NOT_FOUND ||
      error.native_code == ERROR_PATH_NOT_FOUND;
}

clonecore::Result<std::wstring> final_path_from_image_partial(
    const std::wstring& image_partial_path) {
  constexpr std::wstring_view suffix = L".partial";
  constexpr std::wstring_view final_suffix = L".tsumugi";
  if (image_partial_path.size() <= suffix.size() + final_suffix.size() ||
      !equals_ordinal_ignore_case(
          std::wstring_view(image_partial_path).substr(
              image_partial_path.size() - suffix.size()),
          suffix)) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_PATHNAME,
        L"Resume Slot image-create final path",
        L"認証済みimage-partial pathから完成名を一意に導出できません");
  }
  std::wstring final_path = image_partial_path.substr(
      0U, image_partial_path.size() - suffix.size());
  if (final_path.size() <= final_suffix.size() ||
      !equals_ordinal_ignore_case(
          std::wstring_view(final_path).substr(
              final_path.size() - final_suffix.size()),
          final_suffix)) {
    return result_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_PATHNAME,
        L"Resume Slot image-create final path",
        L"認証済みimage-partial pathが.tsumugi隣接形式ではありません");
  }
  return clonecore::Result<std::wstring>::success(std::move(final_path));
}

clonecore::Status rename_open_handle_no_replace(
    const HANDLE handle,
    const std::wstring& destination,
    const std::wstring_view operation) {
  const std::size_t name_bytes = destination.size() * sizeof(wchar_t);
  const std::size_t buffer_bytes =
      offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
  if (name_bytes > (std::numeric_limits<DWORD>::max)() ||
      buffer_bytes > (std::numeric_limits<DWORD>::max)()) {
    return platform_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        std::wstring(operation),
        L"完成名がWindows上限を超えています");
  }
  std::vector<std::byte> buffer(buffer_bytes, std::byte{0});
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(name_bytes);
  std::memcpy(rename->FileName, destination.data(), name_bytes);
  if (!SetFileInformationByHandle(
          handle,
          FileRenameInfo,
          buffer.data(),
          static_cast<DWORD>(buffer.size()))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        GetLastError()));
  }
  return clonecore::success_status();
}

class WindowsResumeSlotPlatform final : public IResumeSlotPlatform {
 public:
  WindowsResumeSlotPlatform(
      ConfiguredPaths paths,
      WindowsResumeDataBackingProbe backing_probe,
      std::optional<WindowsResumeOwnedPartial> create_partial,
      std::vector<WindowsResumeOwnedObject> create_objects)
      : paths_(std::move(paths)),
        backing_probe_(std::move(backing_probe)),
        create_partial_(std::move(create_partial)),
        create_objects_(std::move(create_objects)) {}

  [[nodiscard]] clonecore::Result<ResumeSlotObservation>
  observe_fixed_slot() override {
    auto state = load_state(false);
    if (!state) {
      return clonecore::Result<ResumeSlotObservation>::failure(state.error());
    }
    return clonecore::Result<ResumeSlotObservation>::success(
        std::move(state.value().observation));
  }

  [[nodiscard]] clonecore::Status create_fixed_slot(
      const ResumeSlotRecord& record) override {
    const auto valid = validate_resume_slot_record(record);
    if (!valid) {
      return valid;
    }
    auto before = load_state(true);
    if (!before) {
      return clonecore::Status::failure(before.error());
    }
    if (before.value().checkpoint) {
      return platform_failure(
          clonecore::ErrorCode::access_denied,
          ERROR_FILE_EXISTS,
          L"Resume Slot CREATE_NEW前確認",
          L"既存active.checkpointは上書きしません");
    }
    if (record.owned_partial.has_value() !=
        before.value().observation.observed_owned_partial.has_value() ||
        (record.owned_partial &&
         !partial_bindings_equal(
             *record.owned_partial,
             *before.value().observation.observed_owned_partial))) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot CREATE_NEW owned partial確認",
          L"宣言したowned partialをopened handleで再確認できません");
    }
    if (!owned_object_bindings_equal(
            record.owned_objects,
            before.value().observation.observed_owned_objects)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot CREATE_NEW owned object確認",
          L"宣言したowned objectsをopened handleで再確認できません");
    }
    const std::optional<std::wstring> partial_path = record.owned_partial
        ? std::optional<std::wstring>(
              before.value().partial->partial.canonical_path)
        : std::nullopt;
    const std::vector<std::wstring> object_paths =
        before.value().owned_object_paths;
    auto bytes = serialize_slot_envelope(
        record, partial_path, object_paths);
    if (!bytes) {
      return clonecore::Status::failure(bytes.error());
    }
    auto stage = write_verified_stage(
        paths_.stage, bytes.value(), record, partial_path, object_paths);
    if (!stage) {
      return clonecore::Status::failure(stage.error());
    }
    auto last = load_state(true);
    if (!last || last.value().checkpoint ||
        (record.owned_partial &&
         (!last.value().partial ||
          !partial_bindings_equal(
              *record.owned_partial,
              last.value().partial->partial.binding))) ||
        !owned_object_bindings_equal(
            record.owned_objects,
            last.value().observation.observed_owned_objects)) {
      discard_exact_stage(paths_.stage, stage.value());
      return last
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot CREATE_NEW直前再識別",
                L"checkpointまたはowned partialが直前に変化しました")
          : clonecore::Status::failure(last.error());
    }
    if (!MoveFileExW(
            extended_path(paths_.stage).c_str(),
            extended_path(paths_.checkpoint).c_str(),
            MOVEFILE_WRITE_THROUGH)) {
      const DWORD native_code = GetLastError();
      discard_exact_stage(paths_.stage, stage.value());
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Resume Slot CREATE_NEW確定",
          native_code));
    }
    auto committed = load_state(true);
    if (!committed || !committed.value().checkpoint ||
        !records_equal(committed.value().checkpoint->stored.record, record) ||
        committed.value().checkpoint->stored.partial_path != partial_path ||
        committed.value().checkpoint->stored.owned_object_paths !=
            object_paths ||
        !same_file_object(
            committed.value().checkpoint->file, stage.value().file)) {
      return committed
          ? platform_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Resume Slot CREATE_NEW確定後読戻し",
                L"確定したslotを完全一致で再確認できません")
          : clonecore::Status::failure(committed.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status replace_fixed_slot(
      const Sha256Digest& expected_checkpoint_record_hash,
      const ResumeSlotRecord& next) override {
    const auto valid = validate_resume_slot_record(next);
    if (!valid) {
      return valid;
    }
    auto current = load_state(true);
    if (!current) {
      return clonecore::Status::failure(current.error());
    }
    const auto relationship = validate_replacement(
        current.value(), expected_checkpoint_record_hash, next);
    if (!relationship) {
      return relationship;
    }
    const std::optional<std::wstring> partial_path =
        current.value().checkpoint->stored.partial_path;
    const std::vector<std::wstring> object_paths =
        current.value().checkpoint->stored.owned_object_paths;
    auto bytes = serialize_slot_envelope(next, partial_path, object_paths);
    if (!bytes) {
      return clonecore::Status::failure(bytes.error());
    }
    auto stage = write_verified_stage(
        paths_.stage, bytes.value(), next, partial_path, object_paths);
    if (!stage) {
      return clonecore::Status::failure(stage.error());
    }
    auto last = load_state(true);
    if (!last) {
      discard_exact_stage(paths_.stage, stage.value());
      return clonecore::Status::failure(last.error());
    }
    const auto last_relationship = validate_replacement(
        last.value(), expected_checkpoint_record_hash, next);
    if (!last_relationship ||
        !same_complete_observation(
            current.value().checkpoint->file,
            last.value().checkpoint->file)) {
      discard_exact_stage(paths_.stage, stage.value());
      return last_relationship
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot atomic replace直前再識別",
                L"checkpoint File ID、寸法、link数または時刻が変化しました")
          : last_relationship;
    }
    if (!ReplaceFileW(
            extended_path(paths_.checkpoint).c_str(),
            extended_path(paths_.stage).c_str(),
            nullptr,
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr)) {
      const DWORD native_code = GetLastError();
      discard_exact_stage(paths_.stage, stage.value());
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Resume Slot atomic replace",
          native_code));
    }
    auto committed = load_state(true);
    if (!committed || !committed.value().checkpoint ||
        !records_equal(committed.value().checkpoint->stored.record, next) ||
        committed.value().checkpoint->stored.partial_path != partial_path ||
        committed.value().checkpoint->stored.owned_object_paths !=
            object_paths ||
        !same_file_object(
            committed.value().checkpoint->file, stage.value().file)) {
      return committed
          ? platform_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Resume Slot atomic replace後読戻し",
                L"置換したslotを完全一致で再確認できません")
          : clonecore::Status::failure(committed.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status
  discard_fixed_slot_and_owned_partial(
      const ResumeSlotBinding& binding) override {
    auto state = load_state(false);
    if (!state) {
      return clonecore::Status::failure(state.error());
    }
    if (!state.value().checkpoint) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_NOT_FOUND,
          L"Resume Slot guarded discard",
          L"拘束済みcheckpointが存在しません");
    }
    auto actual = make_resume_slot_binding(
        state.value().checkpoint->stored.record);
    if (!actual || !binding_equal(actual.value(), binding)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot guarded discard binding",
          L"checkpointの完全bindingがreview済みbindingと一致しません");
    }

    clonecore::UniqueHandle checkpoint(CreateFileW(
        extended_path(paths_.checkpoint).c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!checkpoint) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::identity_mismatch,
          L"Resume Slot guarded discard checkpoint open",
          GetLastError()));
    }
    auto checkpoint_before = observe_regular_single_link_file(
        checkpoint.get(), L"Resume Slot guarded discard checkpoint属性");
    if (!checkpoint_before ||
        !same_complete_observation(
            state.value().checkpoint->file, checkpoint_before.value())) {
      return checkpoint_before
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot guarded discard checkpoint再識別",
                L"checkpointがreview後に変化しました")
          : clonecore::Status::failure(checkpoint_before.error());
    }
    auto checkpoint_bytes = read_bounded_file(
        checkpoint.get(),
        kMaximumWindowsResumeSlotBytes,
        L"Resume Slot guarded discard checkpoint読取り");
    if (!checkpoint_bytes) {
      return clonecore::Status::failure(checkpoint_bytes.error());
    }
    auto checkpoint_record = parse_slot_envelope(checkpoint_bytes.value());
    if (!checkpoint_record) {
      return clonecore::Status::failure(checkpoint_record.error());
    }
    auto rebound = make_resume_slot_binding(checkpoint_record.value().record);
    if (!rebound || !binding_equal(rebound.value(), binding)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot guarded discard checkpoint再解析",
          L"削除handleのrecordがreview済みbindingと一致しません");
    }

    clonecore::UniqueHandle partial;
    std::optional<FileObservation> partial_before;
    if (binding.partial_file_object_identity_hash) {
      if (!checkpoint_record.value().partial_path ||
          !state.value().partial) {
        return platform_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_NOT_FOUND,
            L"Resume Slot guarded discard partial",
            L"checkpointが所有するpartialを再openできません");
      }
      partial.reset(CreateFileW(
          extended_path(*checkpoint_record.value().partial_path).c_str(),
          FILE_READ_ATTRIBUTES | DELETE,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr));
      if (!partial) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::identity_mismatch,
            L"Resume Slot guarded discard partial open",
            GetLastError()));
      }
      auto observed = observe_regular_single_link_file(
          partial.get(), L"Resume Slot guarded discard partial属性");
      if (!observed ||
          !same_complete_observation(
              state.value().partial->file, observed.value())) {
        return observed
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard partial再識別",
                  L"owned partialがreview後に変化しました")
            : clonecore::Status::failure(observed.error());
      }
      auto hash = partial_identity_hash(observed.value());
      if (!hash || !detail::digest_equal(
                       hash.value(),
                       *binding.partial_file_object_identity_hash)) {
        return hash
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard partial File ID",
                  L"owned partialのFile ID Hashが一致しません")
            : clonecore::Status::failure(hash.error());
      }
      partial_before = observed.value();
    }

    std::vector<clonecore::UniqueHandle> object_handles;
    std::vector<FileObservation> object_before;
    if (!binding.owned_object_file_bindings.empty()) {
      if (checkpoint_record.value().owned_object_paths.size() !=
              binding.owned_object_file_bindings.size() ||
          state.value().owned_objects.size() !=
              binding.owned_object_file_bindings.size()) {
        return platform_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_NOT_FOUND,
            L"Resume Slot guarded discard owned objects",
            L"checkpointが所有する全objectを再openできません");
      }
      object_handles.reserve(binding.owned_object_file_bindings.size());
      object_before.reserve(binding.owned_object_file_bindings.size());
      for (std::size_t index = 0U;
           index < binding.owned_object_file_bindings.size(); ++index) {
        const auto& review = binding.owned_object_file_bindings[index];
        clonecore::UniqueHandle object(CreateFileW(
            extended_path(
                checkpoint_record.value().owned_object_paths[index]).c_str(),
            FILE_READ_ATTRIBUTES | DELETE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!object) {
          return clonecore::Status::failure(clonecore::make_win32_error(
              clonecore::ErrorCode::identity_mismatch,
              L"Resume Slot guarded discard owned object open",
              GetLastError()));
        }
        auto observed = observe_regular_single_link_file(
            object.get(), L"Resume Slot guarded discard owned object属性");
        if (!observed || !same_complete_observation(
                             state.value().owned_objects[index].file,
                             observed.value())) {
          return observed
              ? platform_failure(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_FILE_INVALID,
                    L"Resume Slot guarded discard owned object再識別",
                    L"owned objectがreview後に変化しました")
              : clonecore::Status::failure(observed.error());
        }
        const auto opened_path = verify_opened_path(
            object.get(),
            checkpoint_record.value().owned_object_paths[index],
            L"Resume Slot guarded discard owned object実体path");
        if (!opened_path) {
          return opened_path;
        }
        auto hash = owned_object_identity_hash(review.role, observed.value());
        if (!hash || !detail::digest_equal(
                         hash.value(), review.file_object_identity_hash)) {
          return hash
              ? platform_failure(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_FILE_INVALID,
                    L"Resume Slot guarded discard owned object File ID",
                    L"owned objectのrole付きFile ID Hashが一致しません")
              : clonecore::Status::failure(hash.error());
        }
        object_before.push_back(observed.value());
        object_handles.push_back(std::move(object));
      }
    }

    const auto paths_stable = verify_runtime_paths();
    if (!paths_stable) {
      return paths_stable;
    }
    const auto backing = require_backing_proof(
        checkpoint_record.value().record, false);
    if (!backing) {
      return backing;
    }
    auto checkpoint_after = observe_regular_single_link_file(
        checkpoint.get(), L"Resume Slot guarded discard直前checkpoint再識別");
    if (!checkpoint_after || !same_complete_observation(
                                 checkpoint_before.value(),
                                 checkpoint_after.value())) {
      return checkpoint_after
          ? platform_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot guarded discard直前checkpoint再識別",
                L"checkpointが削除直前に変化しました")
          : clonecore::Status::failure(checkpoint_after.error());
    }
    if (partial) {
      auto partial_after = observe_regular_single_link_file(
          partial.get(), L"Resume Slot guarded discard直前partial再識別");
      if (!partial_after || !same_complete_observation(
                                *partial_before, partial_after.value())) {
        return partial_after
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard直前partial再識別",
                  L"owned partialが削除直前に変化しました")
            : clonecore::Status::failure(partial_after.error());
      }
    }
    for (std::size_t index = 0U; index < object_handles.size(); ++index) {
      auto object_after = observe_regular_single_link_file(
          object_handles[index].get(),
          L"Resume Slot guarded discard直前owned object再識別");
      if (!object_after || !same_complete_observation(
                               object_before[index], object_after.value())) {
        return object_after
            ? platform_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot guarded discard直前owned object再識別",
                  L"owned objectが削除直前に変化しました")
            : clonecore::Status::failure(object_after.error());
      }
    }

    const auto checkpoint_pending = mark_delete_pending(
        checkpoint.get(), true, L"Resume Slot checkpoint delete-pending");
    if (!checkpoint_pending) {
      return checkpoint_pending;
    }
    bool partial_pending = false;
    if (partial) {
      const auto pending = mark_delete_pending(
          partial.get(), true, L"Resume Slot owned partial delete-pending");
      if (!pending) {
        const auto rollback = mark_delete_pending(
            checkpoint.get(), false, L"Resume Slot checkpoint delete rollback");
        if (!rollback) {
          return platform_failure(
              clonecore::ErrorCode::io_failed,
              rollback.error().native_code,
              L"Resume Slot guarded discard rollback",
              pending.error().message + L" / " + rollback.error().message);
        }
        return pending;
      }
      partial_pending = true;
    }
    std::size_t object_pending_count{};
    for (auto& object : object_handles) {
      const auto pending = mark_delete_pending(
          object.get(), true, L"Resume Slot owned object delete-pending");
      if (!pending) {
        bool rollback_failed = false;
        DWORD rollback_code = ERROR_SUCCESS;
        for (std::size_t index = 0U; index < object_pending_count; ++index) {
          const auto rollback = mark_delete_pending(
              object_handles[index].get(),
              false,
              L"Resume Slot owned object delete rollback");
          if (!rollback && !rollback_failed) {
            rollback_failed = true;
            rollback_code = rollback.error().native_code;
          }
        }
        if (partial_pending) {
          const auto rollback = mark_delete_pending(
              partial.get(), false, L"Resume Slot partial delete rollback");
          if (!rollback && !rollback_failed) {
            rollback_failed = true;
            rollback_code = rollback.error().native_code;
          }
        }
        const auto checkpoint_rollback = mark_delete_pending(
            checkpoint.get(), false, L"Resume Slot checkpoint delete rollback");
        if (!checkpoint_rollback && !rollback_failed) {
          rollback_failed = true;
          rollback_code = checkpoint_rollback.error().native_code;
        }
        if (rollback_failed) {
          return platform_failure(
              clonecore::ErrorCode::io_failed,
              rollback_code,
              L"Resume Slot guarded discard multi-object rollback",
              L"削除予約失敗後に全objectのrollbackを証明できません");
        }
        return pending;
      }
      ++object_pending_count;
    }
    checkpoint.reset();
    partial.reset();
    object_handles.clear();
    if (partial_pending) {
      create_partial_.reset();
    }
    if (object_pending_count != 0U) {
      create_objects_.clear();
    }

    auto after = load_state(false);
    if (!after || after.value().checkpoint || after.value().partial ||
        !after.value().owned_objects.empty()) {
      return after
          ? platform_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_FILE_INVALID,
                L"Resume Slot guarded discard後確認",
                L"checkpointまたはowned partialが削除後も残っています")
          : clonecore::Status::failure(after.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<
      PersistentPeExactImageCreateObservation>
  inspect_persistent_pe_exact_image_create() override {
    auto state = load_persistent_pe_exact_image_create_state();
    if (!state) {
      return clonecore::Result<
          PersistentPeExactImageCreateObservation>::failure(state.error());
    }
    return clonecore::Result<
        PersistentPeExactImageCreateObservation>::success(
        std::move(state.value().public_observation));
  }

  [[nodiscard]] clonecore::Result<
      PersistentPeExactImageCreateCommitReport>
  commit_persistent_pe_exact_image_create(
      const PersistentPeExactImageCreateCommitRequest& request) override {
    auto reviewed_final = canonical_local_path(
        request.reviewed_final_path,
        L"Resume Slot image-create reviewed final");
    if (!reviewed_final || !request.verify_published_image) {
      return reviewed_final
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_FUNCTION,
                L"Resume Slot image-create commit verifier",
                L"完成.tsumugiの全量検証callbackがありません")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                reviewed_final.error());
    }

    auto state = load_persistent_pe_exact_image_create_state();
    if (!state) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(state.error());
    }
    const auto object_state = state.value().public_observation.state;
    if ((object_state !=
             PersistentPeExactImageCreateObjectState::staged &&
         object_state !=
             PersistentPeExactImageCreateObjectState::published &&
         object_state !=
             PersistentPeExactImageCreateObjectState::retirement_pending) ||
        !state.value().public_observation.slot ||
        !state.value().public_observation.binding ||
        !state.value().checkpoint || !state.value().image ||
        !equals_ordinal_ignore_case(
            reviewed_final.value(),
            state.value().public_observation.final_path) ||
        !binding_equal(
            request.reviewed_binding,
            *state.value().public_observation.binding)) {
      return result_failure<PersistentPeExactImageCreateCommitReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot image-create commit binding",
          L"表示後のslot、完成名、または所有object bindingが一致しません");
    }
    const auto& record = *state.value().public_observation.slot;
    const auto& checkpoint_record = record.checkpoint.checkpoint;
    if (checkpoint_record.phase != CheckpointPhase::commit_ready ||
        !checkpoint_record.output_progress_evidence ||
        checkpoint_record.output_progress_evidence->primary_output_length ==
            0U ||
        detail::digest_is_zero(
            checkpoint_record.output_progress_evidence
                ->verified_prefix_hash)) {
      return result_failure<PersistentPeExactImageCreateCommitReport>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_STATE,
          L"Resume Slot image-create commit-ready",
          L"完成名確定に必要なcommit-ready全量Hash/長さがありません");
    }
    if (object_state ==
            PersistentPeExactImageCreateObjectState::staged &&
        (!state.value().public_observation.final_path_available ||
         !request.reprove_before_publish)) {
      return result_failure<PersistentPeExactImageCreateCommitReport>(
          clonecore::ErrorCode::confirmation_required,
          state.value().public_observation.final_path_available
              ? ERROR_INVALID_FUNCTION
              : ERROR_FILE_EXISTS,
          L"Resume Slot image-create pre-publish",
          state.value().public_observation.final_path_available
              ? L"source/destinationの直前再証明callbackがありません"
              : L"完成名が既存objectに占有されているため上書きしません");
    }

    clonecore::UniqueHandle checkpoint(CreateFileW(
        extended_path(paths_.checkpoint).c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!checkpoint) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::identity_mismatch,
              L"Resume Slot image-create checkpoint exact-open",
              GetLastError()));
    }
    auto checkpoint_before = observe_regular_single_link_file(
        checkpoint.get(),
        L"Resume Slot image-create checkpoint exact-open属性");
    if (!checkpoint_before || !same_complete_observation(
                                  state.value().checkpoint->file,
                                  checkpoint_before.value())) {
      return checkpoint_before
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create checkpoint exact-open",
                L"checkpoint File ID/内容属性が表示後に変化しました")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                checkpoint_before.error());
    }
    const auto checkpoint_path = verify_opened_path(
        checkpoint.get(),
        paths_.checkpoint,
        L"Resume Slot image-create checkpoint exact path");
    if (!checkpoint_path) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          checkpoint_path.error());
    }
    auto checkpoint_bytes = read_bounded_file(
        checkpoint.get(),
        kMaximumWindowsResumeSlotBytes,
        L"Resume Slot image-create checkpoint exact readback");
    if (!checkpoint_bytes) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          checkpoint_bytes.error());
    }
    auto fresh_stored = parse_slot_envelope(checkpoint_bytes.value());
    auto fresh_binding = fresh_stored
        ? make_resume_slot_binding(fresh_stored.value().record)
        : clonecore::Result<ResumeSlotBinding>::failure(
              fresh_stored.error());
    if (!fresh_stored || !fresh_binding ||
        !binding_equal(fresh_binding.value(), request.reviewed_binding) ||
        !records_equal(fresh_stored.value().record, record)) {
      return !fresh_stored
          ? clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                fresh_stored.error())
          : !fresh_binding
              ? clonecore::Result<
                    PersistentPeExactImageCreateCommitReport>::failure(
                    fresh_binding.error())
              : result_failure<PersistentPeExactImageCreateCommitReport>(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_FILE_INVALID,
                    L"Resume Slot image-create checkpoint exact binding",
                    L"exact-openしたcheckpointがreview済みrecordと一致しません");
    }

    const bool staged = object_state ==
        PersistentPeExactImageCreateObjectState::staged;
    clonecore::UniqueHandle image(CreateFileW(
        extended_path(state.value().image_path).c_str(),
        GENERIC_READ | (staged ? DELETE : 0U),
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!image) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::identity_mismatch,
              L"Resume Slot image-create image exact-open",
              GetLastError()));
    }
    auto image_before = observe_regular_single_link_file(
        image.get(), L"Resume Slot image-create image exact-open属性");
    if (!image_before || !same_complete_observation(
                             state.value().image->file,
                             image_before.value())) {
      return image_before
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create image exact-open",
                L"image File ID/寸法/時刻が表示後に変化しました")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                image_before.error());
    }
    const auto image_path = verify_opened_path(
        image.get(),
        state.value().image_path,
        L"Resume Slot image-create image exact path");
    if (!image_path) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          image_path.error());
    }
    auto image_hash = owned_object_identity_hash(
        ResumeOwnedObjectRole::image_partial, image_before.value());
    const auto image_review = std::find_if(
        request.reviewed_binding.owned_object_file_bindings.begin(),
        request.reviewed_binding.owned_object_file_bindings.end(),
        [](const ResumeOwnedObjectReviewBinding& value) {
          return value.role == ResumeOwnedObjectRole::image_partial;
        });
    if (!image_hash ||
        image_review ==
            request.reviewed_binding.owned_object_file_bindings.end() ||
        !detail::digest_equal(
            image_hash.value(), image_review->file_object_identity_hash)) {
      return image_hash
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create image exact File ID",
                L"exact-openしたimageが移動前partial File IDと一致しません")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                image_hash.error());
    }

    clonecore::UniqueHandle journal;
    std::optional<FileObservation> journal_before;
    if (state.value().journal) {
      journal.reset(CreateFileW(
          extended_path(state.value().journal_path).c_str(),
          GENERIC_READ | DELETE,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
          nullptr));
      if (!journal) {
        return clonecore::Result<
            PersistentPeExactImageCreateCommitReport>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::identity_mismatch,
                L"Resume Slot image-create journal exact-open",
                GetLastError()));
      }
      auto observed = observe_regular_single_link_file(
          journal.get(),
          L"Resume Slot image-create journal exact-open属性");
      const auto journal_path = verify_opened_path(
          journal.get(),
          state.value().journal_path,
          L"Resume Slot image-create journal exact path");
      if (!observed || !journal_path || !same_complete_observation(
                           state.value().journal->file,
                           observed.value())) {
        return !observed
            ? clonecore::Result<
                  PersistentPeExactImageCreateCommitReport>::failure(
                  observed.error())
            : !journal_path
                ? clonecore::Result<
                      PersistentPeExactImageCreateCommitReport>::failure(
                      journal_path.error())
                : result_failure<
                      PersistentPeExactImageCreateCommitReport>(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_FILE_INVALID,
                      L"Resume Slot image-create journal exact-open",
                      L"journal File ID/寸法/時刻が表示後に変化しました");
      }
      auto journal_hash = owned_object_identity_hash(
          ResumeOwnedObjectRole::image_resume_journal,
          observed.value());
      const auto journal_review = std::find_if(
          request.reviewed_binding.owned_object_file_bindings.begin(),
          request.reviewed_binding.owned_object_file_bindings.end(),
          [](const ResumeOwnedObjectReviewBinding& value) {
            return value.role ==
                ResumeOwnedObjectRole::image_resume_journal;
          });
      if (!journal_hash ||
          journal_review ==
              request.reviewed_binding.owned_object_file_bindings.end() ||
          !detail::digest_equal(
              journal_hash.value(),
              journal_review->file_object_identity_hash)) {
        return journal_hash
            ? result_failure<
                  PersistentPeExactImageCreateCommitReport>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot image-create journal exact File ID",
                  L"exact-openしたjournalがreview済みFile IDと一致しません")
            : clonecore::Result<
                  PersistentPeExactImageCreateCommitReport>::failure(
                  journal_hash.error());
      }
      journal_before = observed.value();
    } else if (object_state !=
               PersistentPeExactImageCreateObjectState::retirement_pending) {
      return result_failure<PersistentPeExactImageCreateCommitReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_NOT_FOUND,
          L"Resume Slot image-create journal exact-open",
          L"retirement途中以外でjournalが存在しません");
    }

    if (staged) {
      clonecore::Status reproof = [&]() {
        try {
          return request.reprove_before_publish();
        } catch (...) {
          return platform_failure(
              clonecore::ErrorCode::internal_error,
              ERROR_UNHANDLED_EXCEPTION,
              L"Resume Slot image-create pre-publish reproof",
              L"source/destination再証明callbackが例外で停止しました");
        }
      }();
      if (!reproof) {
        return clonecore::Result<
            PersistentPeExactImageCreateCommitReport>::failure(
            reproof.error());
      }
      const auto backing_reproof = require_backing_proof(record, true);
      if (!backing_reproof) {
        return clonecore::Result<
            PersistentPeExactImageCreateCommitReport>::failure(
            backing_reproof.error());
      }
      SetLastError(ERROR_SUCCESS);
      if (GetFileAttributesW(
              extended_path(reviewed_final.value()).c_str()) !=
              INVALID_FILE_ATTRIBUTES ||
          (GetLastError() != ERROR_FILE_NOT_FOUND &&
           GetLastError() != ERROR_PATH_NOT_FOUND)) {
        return result_failure<PersistentPeExactImageCreateCommitReport>(
            clonecore::ErrorCode::confirmation_required,
            ERROR_FILE_EXISTS,
            L"Resume Slot image-create final no-overwrite",
            L"完成名が直前に占有されたため上書きしません");
      }
      auto image_stable = observe_regular_single_link_file(
          image.get(),
          L"Resume Slot image-create publish直前image再識別");
      auto journal_stable = observe_regular_single_link_file(
          journal.get(),
          L"Resume Slot image-create publish直前journal再識別");
      auto checkpoint_stable = observe_regular_single_link_file(
          checkpoint.get(),
          L"Resume Slot image-create publish直前checkpoint再識別");
      if (!image_stable || !journal_stable || !checkpoint_stable ||
          !same_complete_observation(
              image_before.value(), image_stable.value()) ||
          !same_complete_observation(
              *journal_before, journal_stable.value()) ||
          !same_complete_observation(
              checkpoint_before.value(), checkpoint_stable.value())) {
        return result_failure<PersistentPeExactImageCreateCommitReport>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot image-create publish直前再識別",
            L"partial/journal/checkpointのいずれかが直前に変化しました");
      }
      const auto renamed = rename_open_handle_no_replace(
          image.get(),
          reviewed_final.value(),
          L"Resume Slot image-create recoverable publish");
      if (!renamed) {
        return clonecore::Result<
            PersistentPeExactImageCreateCommitReport>::failure(
            renamed.error());
      }
      const auto renamed_path = verify_opened_path(
          image.get(),
          reviewed_final.value(),
          L"Resume Slot image-create published exact path");
      auto renamed_identity = observe_regular_single_link_file(
          image.get(),
          L"Resume Slot image-create published exact identity");
      if (!renamed_path || !renamed_identity ||
          !same_content_observation_across_rename(
              image_before.value(), renamed_identity.value())) {
        return !renamed_path
            ? clonecore::Result<
                  PersistentPeExactImageCreateCommitReport>::failure(
                  renamed_path.error())
            : !renamed_identity
                ? clonecore::Result<
                      PersistentPeExactImageCreateCommitReport>::failure(
                      renamed_identity.error())
                : result_failure<
                      PersistentPeExactImageCreateCommitReport>(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_FILE_INVALID,
                      L"Resume Slot image-create published File ID",
                      L"完成名へ移動後のFile IDがpartialと一致しません");
      }
    }

    // Drop the DELETE-capable rename handle, then immediately reacquire a
    // read lock that denies write/delete sharing. Any replacement in this
    // narrow gap is detected by the role-bound File ID before verification.
    image.reset();
    clonecore::UniqueHandle final_lock(CreateFileW(
        extended_path(reviewed_final.value()).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!final_lock) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::identity_mismatch,
              L"Resume Slot image-create published read lock",
              GetLastError()));
    }
    auto final_before = observe_regular_single_link_file(
        final_lock.get(),
        L"Resume Slot image-create published read lock属性");
    const auto final_path = verify_opened_path(
        final_lock.get(),
        reviewed_final.value(),
        L"Resume Slot image-create published read lock path");
    auto final_hash = final_before
        ? owned_object_identity_hash(
              ResumeOwnedObjectRole::image_partial,
              final_before.value())
        : clonecore::Result<Sha256Digest>::failure(final_before.error());
    if (!final_before || !final_path || !final_hash ||
        !same_file_object(image_before.value(), final_before.value()) ||
        !detail::digest_equal(
            final_hash.value(), image_review->file_object_identity_hash)) {
      return !final_before
          ? clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                final_before.error())
          : !final_path
              ? clonecore::Result<
                    PersistentPeExactImageCreateCommitReport>::failure(
                    final_path.error())
              : !final_hash
                  ? clonecore::Result<
                        PersistentPeExactImageCreateCommitReport>::failure(
                        final_hash.error())
                  : result_failure<
                        PersistentPeExactImageCreateCommitReport>(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_FILE_INVALID,
                        L"Resume Slot image-create published read lock",
                        L"全量検証対象が移動前partial File IDと一致しません");
    }

    clonecore::Result<PersistentPeExactImageCreateVerification> verified =
        [&]() {
          try {
            return request.verify_published_image(reviewed_final.value());
          } catch (...) {
            return clonecore::Result<
                PersistentPeExactImageCreateVerification>::failure(
                platform_error(
                    clonecore::ErrorCode::internal_error,
                    ERROR_UNHANDLED_EXCEPTION,
                    L"Resume Slot image-create full verification",
                    L"完成.tsumugi検証callbackが例外で停止しました"));
          }
        }();
    const auto& expected_progress =
        *checkpoint_record.output_progress_evidence;
    if (!verified ||
        verified.value().image_length !=
            expected_progress.primary_output_length ||
        !detail::digest_equal(
            verified.value().global_hash,
            expected_progress.verified_prefix_hash) ||
        !verified.value().header_hash_verified ||
        !verified.value().metadata_authenticated ||
        !verified.value().all_chunks_verified ||
        !verified.value().global_hash_verified) {
      return verified
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Resume Slot image-create full verification",
                L"完成.tsumugiの長さ、全量Hash、認証またはchunk検証がcheckpointと一致しません")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                verified.error());
    }
    auto final_after = observe_regular_single_link_file(
        final_lock.get(),
        L"Resume Slot image-create full verification後File ID");
    if (!final_after || !same_complete_observation(
                            final_before.value(), final_after.value())) {
      return final_after
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create full verification後File ID",
                L"全量検証中に完成file objectが変化しました")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                final_after.error());
    }

    bool journal_retired = !journal;
    if (journal) {
      auto journal_after = observe_regular_single_link_file(
          journal.get(),
          L"Resume Slot image-create journal retire直前");
      if (!journal_after || !same_complete_observation(
                                *journal_before, journal_after.value())) {
        return journal_after
            ? result_failure<
                  PersistentPeExactImageCreateCommitReport>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot image-create journal retire直前",
                  L"journalが全量検証中に変化しました")
            : clonecore::Result<
                  PersistentPeExactImageCreateCommitReport>::failure(
                  journal_after.error());
      }
      const auto pending = mark_delete_pending(
          journal.get(),
          true,
          L"Resume Slot image-create journal exact retire");
      if (!pending) {
        return clonecore::Result<
            PersistentPeExactImageCreateCommitReport>::failure(
            pending.error());
      }
      journal.reset();
      SetLastError(ERROR_SUCCESS);
      if (GetFileAttributesW(
              extended_path(state.value().journal_path).c_str()) !=
              INVALID_FILE_ATTRIBUTES ||
          (GetLastError() != ERROR_FILE_NOT_FOUND &&
           GetLastError() != ERROR_PATH_NOT_FOUND)) {
        return result_failure<PersistentPeExactImageCreateCommitReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_FILE_INVALID,
            L"Resume Slot image-create journal retire readback",
            L"journalのexact retireを不存在として確認できません");
      }
      journal_retired = true;
    }

    auto checkpoint_after = observe_regular_single_link_file(
        checkpoint.get(),
        L"Resume Slot image-create checkpoint retire直前");
    if (!checkpoint_after || !same_complete_observation(
                               checkpoint_before.value(),
                               checkpoint_after.value())) {
      return checkpoint_after
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create checkpoint retire直前",
                L"checkpointが全量検証中に変化しました")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                checkpoint_after.error());
    }
    const auto checkpoint_pending = mark_delete_pending(
        checkpoint.get(),
        true,
        L"Resume Slot image-create checkpoint exact retire");
    if (!checkpoint_pending) {
      return clonecore::Result<
          PersistentPeExactImageCreateCommitReport>::failure(
          checkpoint_pending.error());
    }
    checkpoint.reset();
    SetLastError(ERROR_SUCCESS);
    if (GetFileAttributesW(extended_path(paths_.checkpoint).c_str()) !=
            INVALID_FILE_ATTRIBUTES ||
        (GetLastError() != ERROR_FILE_NOT_FOUND &&
         GetLastError() != ERROR_PATH_NOT_FOUND)) {
      return result_failure<PersistentPeExactImageCreateCommitReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_FILE_INVALID,
          L"Resume Slot image-create checkpoint retire readback",
          L"checkpointのexact retireを不存在として確認できません");
    }
    auto final_end = observe_regular_single_link_file(
        final_lock.get(),
        L"Resume Slot image-create retire後final再識別");
    if (!final_end || !same_complete_observation(
                          final_before.value(), final_end.value())) {
      return final_end
          ? result_failure<PersistentPeExactImageCreateCommitReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create retire後final再識別",
                L"transaction退役中に完成.tsumugiが変化しました")
          : clonecore::Result<
                PersistentPeExactImageCreateCommitReport>::failure(
                final_end.error());
    }
    create_objects_.clear();
    return clonecore::Result<
        PersistentPeExactImageCreateCommitReport>::success({
        .recovered_after_publish = !staged,
        .image_published = true,
        .complete_image_verified = true,
        .journal_retired = journal_retired,
        .slot_retired = true,
    });
  }

 private:
  [[nodiscard]] clonecore::Result<PersistentPeImageCreatePlatformState>
  load_persistent_pe_exact_image_create_state() {
    const auto paths_stable = verify_runtime_paths();
    if (!paths_stable) {
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::failure(
          paths_stable.error());
    }
    auto checkpoint = read_slot_file(paths_.checkpoint);
    if (!checkpoint) {
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::failure(
          checkpoint.error());
    }
    if (!checkpoint.value()) {
      const auto backing = require_backing_proof(std::nullopt, false);
      if (!backing) {
        return clonecore::Result<
            PersistentPeImageCreatePlatformState>::failure(backing.error());
      }
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::success({
          .public_observation = {
              .state =
                  PersistentPeExactImageCreateObjectState::no_slot,
          },
      });
    }

    auto stored = std::move(*checkpoint.value());
    const auto backing = require_backing_proof(stored.stored.record, false);
    if (!backing) {
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::failure(backing.error());
    }
    auto binding = make_resume_slot_binding(stored.stored.record);
    if (!binding) {
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::failure(binding.error());
    }
    if (stored.stored.record.capability !=
        ResumeCapability::persistent_pe_exact_image_create) {
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::success({
          .public_observation = {
              .state = PersistentPeExactImageCreateObjectState::
                  other_capability,
              .slot = stored.stored.record,
              .binding = binding.take_value(),
          },
          .checkpoint = std::move(stored),
      });
    }
    if (stored.stored.record.owned_objects.size() != 2U ||
        stored.stored.owned_object_paths.size() != 2U ||
        stored.stored.record.checkpoint.checkpoint.schema_version !=
            kCheckpointSchemaVersionV3) {
      return result_failure<PersistentPeImageCreatePlatformState>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot image-create object set",
          L"capability-8 slotがschema v3のpartial+journal二者束縛ではありません");
    }

    std::optional<std::size_t> image_index;
    std::optional<std::size_t> journal_index;
    for (std::size_t index = 0U;
         index < stored.stored.record.owned_objects.size(); ++index) {
      switch (stored.stored.record.owned_objects[index].role) {
        case ResumeOwnedObjectRole::image_partial:
          image_index = index;
          break;
        case ResumeOwnedObjectRole::image_resume_journal:
          journal_index = index;
          break;
        case ResumeOwnedObjectRole::rescue_stage:
          return result_failure<PersistentPeImageCreatePlatformState>(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Resume Slot image-create object role",
              L"通常image-create slotにrescue stageが含まれています");
      }
    }
    if (!image_index || !journal_index) {
      return result_failure<PersistentPeImageCreatePlatformState>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot image-create object role",
          L"image partialまたはresume journalのroleがありません");
    }

    const std::wstring image_partial_path =
        stored.stored.owned_object_paths[*image_index];
    const std::wstring journal_path =
        stored.stored.owned_object_paths[*journal_index];
    auto final_path = final_path_from_image_partial(image_partial_path);
    if (!final_path) {
      return clonecore::Result<
          PersistentPeImageCreatePlatformState>::failure(final_path.error());
    }

    bool published = false;
    auto image = observe_owned_object(
        image_partial_path,
        ResumeOwnedObjectRole::image_partial,
        stored.stored.record.checkpoint.checkpoint.operation_id,
        stored.stored.record.identities);
    if (!image && file_not_found_error(image.error())) {
      image = observe_owned_object(
          final_path.value(),
          ResumeOwnedObjectRole::image_partial,
          stored.stored.record.checkpoint.checkpoint.operation_id,
          stored.stored.record.identities);
      published = true;
    }
    if (!image || !owned_object_binding_equal(
                      image.value().object.binding,
                      stored.stored.record.owned_objects[*image_index])) {
      return image
          ? result_failure<PersistentPeImageCreatePlatformState>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create image File ID",
                L"staged/final image objectがslotの移動前File IDと一致しません")
          : clonecore::Result<
                PersistentPeImageCreatePlatformState>::failure(image.error());
    }
    if (published &&
        stored.stored.record.checkpoint.checkpoint.phase !=
            CheckpointPhase::commit_ready) {
      return result_failure<PersistentPeImageCreatePlatformState>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_STATE,
          L"Resume Slot image-create published phase",
          L"commit-ready前のcheckpointから完成名への移動を採用しません");
    }

    auto journal = observe_owned_object(
        journal_path,
        ResumeOwnedObjectRole::image_resume_journal,
        stored.stored.record.checkpoint.checkpoint.operation_id,
        stored.stored.record.identities);
    bool retirement_pending = false;
    if (!journal && file_not_found_error(journal.error()) && published &&
        stored.stored.record.checkpoint.checkpoint.phase ==
            CheckpointPhase::commit_ready) {
      retirement_pending = true;
    } else if (!journal || !owned_object_binding_equal(
                            journal.value().object.binding,
                            stored.stored.record.owned_objects[
                                *journal_index])) {
      return journal
          ? result_failure<PersistentPeImageCreatePlatformState>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot image-create journal File ID",
                L"resume journalがslotのFile IDと一致しません")
          : clonecore::Result<
                PersistentPeImageCreatePlatformState>::failure(
                journal.error());
    }

    bool final_path_available = false;
    if (!published) {
      SetLastError(ERROR_SUCCESS);
      const DWORD attributes =
          GetFileAttributesW(extended_path(final_path.value()).c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD native_code = GetLastError();
        if (native_code != ERROR_FILE_NOT_FOUND &&
            native_code != ERROR_PATH_NOT_FOUND) {
          return clonecore::Result<
              PersistentPeImageCreatePlatformState>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::query_failed,
                  L"Resume Slot image-create final absence",
                  native_code));
        }
        final_path_available = true;
      }
    }

    PersistentPeImageCreatePlatformState result{
        .public_observation = {
            .state = retirement_pending
                ? PersistentPeExactImageCreateObjectState::retirement_pending
                : published
                    ? PersistentPeExactImageCreateObjectState::published
                    : PersistentPeExactImageCreateObjectState::staged,
            .slot = stored.stored.record,
            .binding = binding.take_value(),
            .final_path = final_path.value(),
            .final_path_available = final_path_available,
        },
        .checkpoint = std::move(stored),
        .image = image.take_value(),
        .image_path = published ? final_path.value() : image_partial_path,
        .journal_path = journal_path,
    };
    if (journal) {
      result.journal = journal.take_value();
    }
    return clonecore::Result<
        PersistentPeImageCreatePlatformState>::success(std::move(result));
  }

  [[nodiscard]] clonecore::Status verify_runtime_paths() const {
    const auto application = verify_directory_chain(
        paths_.application_directory,
        L"Resume Slot application chain再確認");
    if (!application) {
      return application;
    }
    const auto executable = verify_regular_executable(paths_.executable);
    if (!executable) {
      return executable;
    }
    return verify_directory_chain(
        paths_.data_directory,
        L"Resume Slot EXE隣data chain再確認");
  }

  [[nodiscard]] clonecore::Status require_backing_proof(
      const std::optional<ResumeSlotRecord>& record,
      const bool require_source_separation) {
    clonecore::Result<WindowsResumeDataBackingProof> proof = [&]() {
      try {
        return backing_probe_(paths_.data_directory, record);
      } catch (...) {
        return result_failure<WindowsResumeDataBackingProof>(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"Resume Slot data backing proof",
            L"呼出側proof callbackが例外を送出しました");
      }
    }();
    if (!proof) {
      return clonecore::Status::failure(proof.error());
    }
    if (!proof.value().identity_from_open_handle ||
        (require_source_separation &&
         !proof.value().separated_from_source) ||
        detail::digest_is_zero(
            proof.value().backing_storage_identity_hash)) {
      return platform_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Resume Slot data backing proof",
        require_source_separation
            ? L"opened handle由来の非ゼロ識別とsource backing分離を証明できません"
            : L"opened handle由来の非ゼロdata backing識別を証明できません");
    }
    if (bound_backing_identity_ &&
        !detail::digest_equal(
            *bound_backing_identity_,
            proof.value().backing_storage_identity_hash)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot data backing再識別",
          L"adapter初回観測後にdata backing identityが変化しました");
    }
    if (!bound_backing_identity_) {
      bound_backing_identity_ = proof.value().backing_storage_identity_hash;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<PlatformState> load_state(
      const bool require_source_separation) {
    const auto before_paths = verify_runtime_paths();
    if (!before_paths) {
      return clonecore::Result<PlatformState>::failure(before_paths.error());
    }
    auto checkpoint = read_slot_file(paths_.checkpoint);
    if (!checkpoint) {
      return clonecore::Result<PlatformState>::failure(checkpoint.error());
    }
    const std::optional<ResumeSlotRecord> record = checkpoint.value()
        ? std::optional<ResumeSlotRecord>(checkpoint.value()->stored.record)
        : std::nullopt;
    const auto backing = require_backing_proof(
        record, require_source_separation);
    if (!backing) {
      return clonecore::Result<PlatformState>::failure(backing.error());
    }

    std::optional<PartialObservation> partial;
    std::optional<std::wstring> partial_path;
    if (checkpoint.value() && checkpoint.value()->stored.record.owned_partial) {
      if (!checkpoint.value()->stored.partial_path) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Resume Slot persisted partial path",
            L"owned partial bindingに対応するpathがありません");
      }
      if (create_partial_ &&
          (!equals_ordinal_ignore_case(
               create_partial_->canonical_path,
               *checkpoint.value()->stored.partial_path) ||
           !partial_bindings_equal(
               create_partial_->binding,
               *checkpoint.value()->stored.record.owned_partial))) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot persisted/create partial競合",
            L"persist済みpartialとcreate用partialが一致しません");
      }
      auto observed = observe_owned_partial(
          *checkpoint.value()->stored.partial_path,
          checkpoint.value()->stored.record.owned_partial->operation_id,
          checkpoint.value()->stored.record.owned_partial->identities);
      if (!observed) {
        return clonecore::Result<PlatformState>::failure(observed.error());
      }
      if (!partial_bindings_equal(
              observed.value().partial.binding,
              *checkpoint.value()->stored.record.owned_partial)) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot persisted partial File ID",
            L"persist済みowned partialが別file objectへ差し替えられました");
      }
      partial_path = checkpoint.value()->stored.partial_path;
      partial = observed.take_value();
    } else if (create_partial_) {
      auto observed = observe_owned_partial(
          create_partial_->canonical_path,
          create_partial_->binding.operation_id,
          create_partial_->binding.identities);
      if (!observed) {
        return clonecore::Result<PlatformState>::failure(observed.error());
      }
      if (!partial_bindings_equal(
              observed.value().partial.binding,
              create_partial_->binding)) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot create partial File ID",
            L"create用owned partialが別file objectへ差し替えられました");
      }
      partial_path = create_partial_->canonical_path;
      partial = observed.take_value();
    }

    std::vector<OwnedObjectObservation> owned_objects;
    std::vector<std::wstring> owned_object_paths;
    if (checkpoint.value() &&
        !checkpoint.value()->stored.record.owned_objects.empty()) {
      const auto& persisted_objects =
          checkpoint.value()->stored.record.owned_objects;
      const auto& persisted_paths =
          checkpoint.value()->stored.owned_object_paths;
      if (persisted_objects.size() != persisted_paths.size()) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Resume Slot persisted owned object path",
            L"owned object bindingとpathの件数が一致しません");
      }
      if (!create_objects_.empty()) {
        if (create_objects_.size() != persisted_objects.size()) {
          return result_failure<PlatformState>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot persisted/create owned object競合",
              L"persist済みobjectとcreate用objectの件数が一致しません");
        }
        for (std::size_t index = 0U; index < persisted_objects.size(); ++index) {
          if (!equals_ordinal_ignore_case(
                  create_objects_[index].canonical_path,
                  persisted_paths[index]) ||
              !owned_object_bindings_equal(
                  {create_objects_[index].binding},
                  {persisted_objects[index]})) {
            return result_failure<PlatformState>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Resume Slot persisted/create owned object競合",
                L"persist済みobjectとcreate用objectが一致しません");
          }
        }
      }
      owned_objects.reserve(persisted_objects.size());
      owned_object_paths.reserve(persisted_paths.size());
      for (std::size_t index = 0U; index < persisted_objects.size(); ++index) {
        const auto& expected = persisted_objects[index];
        auto observed = observe_owned_object(
            persisted_paths[index],
            expected.role,
            expected.operation_id,
            expected.identities);
        if (!observed) {
          return clonecore::Result<PlatformState>::failure(observed.error());
        }
        if (!owned_object_bindings_equal(
                {observed.value().object.binding}, {expected})) {
          return result_failure<PlatformState>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot persisted owned object File ID",
              L"persist済みowned objectが別file objectへ差し替えられました");
        }
        owned_object_paths.push_back(persisted_paths[index]);
        owned_objects.push_back(observed.take_value());
      }
    } else if (!create_objects_.empty()) {
      if (checkpoint.value()) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"Resume Slot legacy/create owned object競合",
            L"既存slotがcreate用owned objectsを宣言していません");
      }
      owned_objects.reserve(create_objects_.size());
      owned_object_paths.reserve(create_objects_.size());
      for (const auto& configured : create_objects_) {
        auto observed = observe_owned_object(
            configured.canonical_path,
            configured.binding.role,
            configured.binding.operation_id,
            configured.binding.identities);
        if (!observed) {
          return clonecore::Result<PlatformState>::failure(observed.error());
        }
        if (!owned_object_bindings_equal(
                {observed.value().object.binding}, {configured.binding})) {
          return result_failure<PlatformState>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot create owned object File ID",
              L"create用owned objectがbinding後に変化しました");
        }
        owned_object_paths.push_back(configured.canonical_path);
        owned_objects.push_back(observed.take_value());
      }
    }

    if (partial_path &&
        (equals_ordinal_ignore_case(*partial_path, paths_.checkpoint) ||
         equals_ordinal_ignore_case(*partial_path, paths_.stage))) {
      return result_failure<PlatformState>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_NAME,
          L"Resume Slot checkpoint/partial分離",
          L"checkpoint、stage、owned partialは別pathでなければなりません");
    }
    for (std::size_t index = 0U; index < owned_object_paths.size(); ++index) {
      const auto& object_path = owned_object_paths[index];
      if (equals_ordinal_ignore_case(object_path, paths_.checkpoint) ||
          equals_ordinal_ignore_case(object_path, paths_.stage) ||
          (partial_path &&
           equals_ordinal_ignore_case(object_path, *partial_path))) {
        return result_failure<PlatformState>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_INVALID_NAME,
            L"Resume Slot checkpoint/owned object分離",
            L"checkpoint、stage、legacy partial、owned objectsは別pathでなければなりません");
      }
      for (std::size_t prior = 0U; prior < index; ++prior) {
        if (equals_ordinal_ignore_case(
                object_path, owned_object_paths[prior])) {
          return result_failure<PlatformState>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_DUP_NAME,
              L"Resume Slot owned object path分離",
              L"owned objectsはそれぞれ別pathでなければなりません");
        }
      }
    }
    const auto after_paths = verify_runtime_paths();
    if (!after_paths) {
      return clonecore::Result<PlatformState>::failure(after_paths.error());
    }
    std::vector<ResumeFileStorageProof> owned_object_files;
    std::vector<ResumeOwnedObjectBinding> observed_owned_objects;
    owned_object_files.reserve(owned_objects.size());
    observed_owned_objects.reserve(owned_objects.size());
    for (const auto& object : owned_objects) {
      owned_object_files.push_back({
          .exists = true,
          .is_regular_file = true,
          .is_reparse_free = true,
          .hard_link_count = 1U,
      });
      observed_owned_objects.push_back(object.object.binding);
    }
    ResumeSlotObservation observation{
        .storage = {
            .checkpoint_path = paths_.checkpoint,
            .paths_are_canonical_local = true,
            .parent_chain_reparse_free = true,
            .placement_separated_from_source = true,
            .checkpoint_and_partial_paths_distinct = true,
            .checkpoint_file = {
                .exists = checkpoint.value().has_value(),
                .is_regular_file = checkpoint.value().has_value(),
                .is_reparse_free = checkpoint.value().has_value(),
                .hard_link_count = checkpoint.value() ? 1U : 0U,
            },
            .owned_partial_file = {
                .exists = partial.has_value(),
                .is_regular_file = partial.has_value(),
                .is_reparse_free = partial.has_value(),
                .hard_link_count = partial ? 1U : 0U,
            },
            .owned_object_files = std::move(owned_object_files),
        },
        .slot = record,
        .observed_owned_partial = partial
            ? std::optional<ResumeOwnedPartialBinding>(partial->partial.binding)
            : std::nullopt,
        .observed_owned_objects = std::move(observed_owned_objects),
    };
    return clonecore::Result<PlatformState>::success(PlatformState{
        .observation = std::move(observation),
        .checkpoint = checkpoint.take_value(),
        .partial = std::move(partial),
        .partial_path = std::move(partial_path),
        .owned_objects = std::move(owned_objects),
        .owned_object_paths = std::move(owned_object_paths),
    });
  }

  [[nodiscard]] clonecore::Status validate_replacement(
      const PlatformState& state,
      const Sha256Digest& expected_checkpoint_record_hash,
      const ResumeSlotRecord& next) const {
    if (!state.checkpoint ||
        !detail::digest_equal(
            state.checkpoint->stored.record.checkpoint.record_hash,
            expected_checkpoint_record_hash)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot replace前record Hash",
          L"既存checkpointがreview済みrecord Hashと一致しません");
    }
    const ResumeSlotRecord& current = state.checkpoint->stored.record;
    if (current.capability != next.capability ||
        !identities_equal(current.identities, next.identities) ||
        !optional_partial_bindings_equal(
            current.owned_partial, next.owned_partial) ||
        !owned_object_bindings_equal(
            current.owned_objects, next.owned_objects)) {
      return platform_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot replace immutable binding",
          L"capability、source/target/outputまたはowned partialが変化しました");
    }
    return validate_checkpoint_transition(
        current.checkpoint.checkpoint,
        next.checkpoint.checkpoint);
  }

  ConfiguredPaths paths_;
  WindowsResumeDataBackingProbe backing_probe_;
  std::optional<WindowsResumeOwnedPartial> create_partial_;
  std::vector<WindowsResumeOwnedObject> create_objects_;
  std::optional<Sha256Digest> bound_backing_identity_;
};

clonecore::Result<std::wstring> current_executable_path() {
  std::vector<wchar_t> buffer(1024U, L'\0');
  for (;;) {
    SetLastError(ERROR_SUCCESS);
    const DWORD copied = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0U) {
      return clonecore::Result<std::wstring>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"Resume Slot current EXE path",
              GetLastError()));
    }
    if (copied < buffer.size()) {
      return clonecore::Result<std::wstring>::success(
          std::wstring(buffer.data(), copied));
    }
    if (buffer.size() >= kMaximumPathCharacters) {
      return result_failure<std::wstring>(
          clonecore::ErrorCode::query_failed,
          ERROR_INSUFFICIENT_BUFFER,
          L"Resume Slot current EXE path",
          L"現在のEXE完全pathを有界bufferで取得できません");
    }
    buffer.resize(
        (std::min)(buffer.size() * 2U, kMaximumPathCharacters), L'\0');
  }
}

}  // namespace

clonecore::Result<WindowsResumeOwnedPartial>
bind_windows_resume_owned_partial(
    const std::wstring& path,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities) {
  try {
    auto canonical = canonical_local_path(
        path, L"Resume Slot owned partial binding path");
    if (!canonical) {
      return clonecore::Result<WindowsResumeOwnedPartial>::failure(
          canonical.error());
    }
    if (canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return result_failure<WindowsResumeOwnedPartial>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_BAD_PATHNAME,
          L"Resume Slot owned partial binding path",
          L"所有対象は.partial拡張子でなければなりません");
    }
    auto observed = observe_owned_partial(
        canonical.value(), operation_id, identities);
    if (!observed) {
      return clonecore::Result<WindowsResumeOwnedPartial>::failure(
          observed.error());
    }
    return clonecore::Result<WindowsResumeOwnedPartial>::success(
        std::move(observed.value().partial));
  } catch (const std::bad_alloc&) {
    return result_failure<WindowsResumeOwnedPartial>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot owned partial binding",
        L"有界識別に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<WindowsResumeOwnedPartial>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot owned partial binding",
        L"owned partialを安全に識別できません");
  }
}

clonecore::Result<WindowsResumeOwnedObject>
bind_windows_resume_owned_object(
    const std::wstring& path,
    const ResumeOwnedObjectRole role,
    const OperationId& operation_id,
    const ResumeIdentityBinding& identities) {
  try {
    if (!known_owned_object_role(role)) {
      return result_failure<WindowsResumeOwnedObject>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Resume Slot owned object binding role",
          L"未対応のowned object roleです");
    }
    auto canonical = canonical_local_path(
        path, L"Resume Slot owned object binding path");
    if (!canonical) {
      return clonecore::Result<WindowsResumeOwnedObject>::failure(
          canonical.error());
    }
    if (canonical.value().size() < 8U ||
        !equals_ordinal_ignore_case(
            std::wstring_view(canonical.value()).substr(
                canonical.value().size() - 8U),
            L".partial")) {
      return result_failure<WindowsResumeOwnedObject>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_BAD_PATHNAME,
          L"Resume Slot owned object binding path",
          L"所有対象は.partial拡張子でなければなりません");
    }
    auto observed = observe_owned_object(
        canonical.value(), role, operation_id, identities);
    if (!observed) {
      return clonecore::Result<WindowsResumeOwnedObject>::failure(
          observed.error());
    }
    return clonecore::Result<WindowsResumeOwnedObject>::success(
        std::move(observed.value().object));
  } catch (const std::bad_alloc&) {
    return result_failure<WindowsResumeOwnedObject>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot owned object binding",
        L"有界識別に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<WindowsResumeOwnedObject>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot owned object binding",
        L"owned objectを安全に識別できません");
  }
}

clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>
make_windows_resume_slot_platform(
    WindowsResumeSlotPlatformOptions options) {
  try {
    if (!options.prove_data_backing_separation) {
      return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Resume Slot platform構成",
          L"data backing分離proof callbackが必要です");
    }
    if ((options.owned_partial_for_create &&
         !options.owned_objects_for_create.empty()) ||
        options.owned_objects_for_create.size() >
            kMaximumResumeOwnedObjects) {
      return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"Resume Slot platform owned object構成",
          L"legacy partialとmulti-objectは併用できず、object数は安全上限以下が必要です");
    }
    auto paths = configure_paths(options.executable_path);
    if (!paths) {
      return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
          paths.error());
    }
    if (options.owned_partial_for_create) {
      auto canonical = canonical_local_path(
          options.owned_partial_for_create->canonical_path,
          L"Resume Slot create partial path");
      if (!canonical) {
        return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
            canonical.error());
      }
      options.owned_partial_for_create->canonical_path =
          canonical.take_value();
      auto observed = observe_owned_partial(
          options.owned_partial_for_create->canonical_path,
          options.owned_partial_for_create->binding.operation_id,
          options.owned_partial_for_create->binding.identities);
      if (!observed ||
          !partial_bindings_equal(
              observed.value().partial.binding,
              options.owned_partial_for_create->binding)) {
        return observed
            ? result_failure<std::unique_ptr<IResumeSlotPlatform>>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot create partial初期再識別",
                  L"create用partialがbinding後に変化しました")
            : clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
                  observed.error());
      }
      if (equals_ordinal_ignore_case(
              options.owned_partial_for_create->canonical_path,
              paths.value().checkpoint) ||
          equals_ordinal_ignore_case(
              options.owned_partial_for_create->canonical_path,
              paths.value().stage)) {
        return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_INVALID_NAME,
            L"Resume Slot checkpoint/partial分離",
            L"checkpoint、stage、owned partialは別pathでなければなりません");
      }
    }
    for (std::size_t index = 0U;
         index < options.owned_objects_for_create.size(); ++index) {
      auto& configured = options.owned_objects_for_create[index];
      if (!known_owned_object_role(configured.binding.role) ||
          (index != 0U &&
           static_cast<std::uint8_t>(
               options.owned_objects_for_create[index - 1U].binding.role) >=
               static_cast<std::uint8_t>(configured.binding.role))) {
        return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DATA,
            L"Resume Slot create owned object role",
            L"known roleを1回ずつ昇順で指定する必要があります");
      }
      auto canonical = canonical_local_path(
          configured.canonical_path,
          L"Resume Slot create owned object path");
      if (!canonical || canonical.value().size() < 8U ||
          !equals_ordinal_ignore_case(
              std::wstring_view(canonical.value()).substr(
                  canonical.value().size() - 8U),
              L".partial")) {
        return canonical
            ? result_failure<std::unique_ptr<IResumeSlotPlatform>>(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_BAD_PATHNAME,
                  L"Resume Slot create owned object path",
                  L"所有対象は.partial拡張子でなければなりません")
            : clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
                  canonical.error());
      }
      configured.canonical_path = canonical.take_value();
      if (equals_ordinal_ignore_case(
              configured.canonical_path, paths.value().checkpoint) ||
          equals_ordinal_ignore_case(
              configured.canonical_path, paths.value().stage)) {
        return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_INVALID_NAME,
            L"Resume Slot checkpoint/owned object分離",
            L"checkpoint、stage、owned objectsは別pathでなければなりません");
      }
      for (std::size_t prior = 0U; prior < index; ++prior) {
        if (equals_ordinal_ignore_case(
                configured.canonical_path,
                options.owned_objects_for_create[prior].canonical_path)) {
          return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_DUP_NAME,
              L"Resume Slot create owned object path",
              L"同じpathを複数roleに使用できません");
        }
      }
      auto observed = observe_owned_object(
          configured.canonical_path,
          configured.binding.role,
          configured.binding.operation_id,
          configured.binding.identities);
      if (!observed || !owned_object_bindings_equal(
                           {observed.value().object.binding},
                           {configured.binding})) {
        return observed
            ? result_failure<std::unique_ptr<IResumeSlotPlatform>>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Resume Slot create owned object初期再識別",
                  L"create用owned objectがbinding後に変化しました")
            : clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
                  observed.error());
      }
    }
    std::unique_ptr<IResumeSlotPlatform> platform =
        std::make_unique<WindowsResumeSlotPlatform>(
            paths.take_value(),
            std::move(options.prove_data_backing_separation),
            std::move(options.owned_partial_for_create),
            std::move(options.owned_objects_for_create));
    return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::success(
        std::move(platform));
  } catch (const std::bad_alloc&) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot platform構成",
        L"adapter構成に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot platform構成",
        L"production adapterを安全に構成できません");
  }
}

clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>
make_current_executable_windows_resume_slot_platform(
    WindowsResumeDataBackingProbe prove_data_backing_separation,
    std::optional<WindowsResumeOwnedPartial> owned_partial_for_create,
    std::vector<WindowsResumeOwnedObject> owned_objects_for_create) {
  try {
    auto executable = current_executable_path();
    if (!executable) {
      return clonecore::Result<std::unique_ptr<IResumeSlotPlatform>>::failure(
          executable.error());
    }
    return make_windows_resume_slot_platform({
        .executable_path = executable.take_value(),
        .prove_data_backing_separation =
            std::move(prove_data_backing_separation),
        .owned_partial_for_create = std::move(owned_partial_for_create),
        .owned_objects_for_create = std::move(owned_objects_for_create),
    });
  } catch (const std::bad_alloc&) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot current EXE platform構成",
        L"adapter構成に必要なメモリを確保できません");
  } catch (...) {
    return result_failure<std::unique_ptr<IResumeSlotPlatform>>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Resume Slot current EXE platform構成",
        L"current EXE用adapterを安全に構成できません");
  }
}

}  // namespace ytec::operationcore

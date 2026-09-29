#include "ytec/windowsapp/support_zip.h"

#include "ytec/windowsapp/log_retention.h"

#include "ytec/clonecore/crc32.h"
#include "ytec/clonecore/log_privacy.h"
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

namespace ytec::windowsapp {

class SupportZipPlanBuilder final {
 public:
  [[nodiscard]] static clonecore::Result<SupportZipPlan> build(
      const std::wstring& executable_path,
      const std::wstring& final_zip_path);
};

namespace {

constexpr std::size_t kMaximumWindowsPathCharacters = 32U * 1024U;
constexpr std::array<std::byte, 3U> kUtf8Bom{
    std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};
constexpr std::uint32_t kZipLocalHeaderSignature = 0x04034B50U;
constexpr std::uint32_t kZipCentralHeaderSignature = 0x02014B50U;
constexpr std::uint32_t kZipEndSignature = 0x06054B50U;
constexpr std::uint16_t kZipVersion20 = 20U;
constexpr std::uint16_t kZipUtf8Flag = 0x0800U;
constexpr std::uint16_t kZipStoredMethod = 0U;
constexpr std::size_t kZipLocalFixedBytes = 30U;
constexpr std::size_t kZipCentralFixedBytes = 46U;
constexpr std::size_t kZipEndBytes = 22U;

clonecore::Error support_error(
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

template <typename T>
clonecore::Result<T> support_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(support_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool ordinal_equal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size() ||
      left.size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool ordinal_less_ignore_case(
    const std::wstring& left,
    const std::wstring& right) noexcept {
  if (left.size() <=
          static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
      right.size() <=
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_LESS_THAN;
  }
  return left.size() < right.size();
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& path,
    const std::wstring_view operation) {
  if (path.size() < 3U || path.size() >= kMaximumWindowsPathCharacters ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'/') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      path.find(L'\0') != std::wstring::npos ||
      (path.size() > 3U && path.ends_with(L"\\")) ||
      path.ends_with(L" ") || path.ends_with(L".")) {
    return support_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"正規化済みローカル絶対パスが必要です");
  }

  std::size_t component_begin = 3U;
  while (component_begin < path.size()) {
    const std::size_t separator = path.find(L'\\', component_begin);
    const std::size_t component_end =
        separator == std::wstring::npos ? path.size() : separator;
    const std::wstring_view component(
        path.data() + component_begin, component_end - component_begin);
    if (component.empty() || component == L"." || component == L".." ||
        component.ends_with(L" ") || component.ends_with(L".")) {
      return support_failure<std::wstring>(
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

  std::vector<wchar_t> resolved(kMaximumWindowsPathCharacters, L'\0');
  wchar_t* file_part = nullptr;
  const DWORD length = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(resolved.size()),
      resolved.data(),
      &file_part);
  if (length == 0U ||
      static_cast<std::size_t>(length) >= resolved.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  const std::wstring canonical(resolved.data(), length);
  if (!ordinal_equal_ignore_case(path, canonical)) {
    return support_failure<std::wstring>(
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
    return support_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        std::wstring(operation),
        L"安全な親ディレクトリを導出できません");
  }
  return clonecore::Result<std::wstring>::success(
      separator == 2U ? path.substr(0U, 3U) : path.substr(0U, separator));
}

clonecore::Result<std::wstring> child_path(
    const std::wstring& parent,
    const std::wstring_view child,
    const std::wstring_view operation) {
  const bool root = parent.ends_with(L"\\");
  const std::size_t separator = root ? 0U : 1U;
  if (parent.size() + separator + child.size() >=
      kMaximumWindowsPathCharacters) {
    return support_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        std::wstring(operation),
        L"導出パスがWindows上限を超えます");
  }
  return clonecore::Result<std::wstring>::success(
      parent + (root ? L"" : L"\\") + std::wstring(child));
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

bool path_inside_or_equal(
    const std::wstring& candidate,
    const std::wstring& parent) noexcept {
  if (candidate.size() < parent.size() ||
      !ordinal_equal_ignore_case(
          std::wstring_view(candidate).substr(0U, parent.size()), parent)) {
    return false;
  }
  return candidate.size() == parent.size() || parent.ends_with(L"\\") ||
         candidate[parent.size()] == L'\\';
}

bool contains_appdata_component(const std::wstring& path) noexcept {
  std::size_t begin = 3U;
  while (begin <= path.size()) {
    const std::size_t separator = path.find(L'\\', begin);
    const std::size_t end =
        separator == std::wstring::npos ? path.size() : separator;
    if (ordinal_equal_ignore_case(
            std::wstring_view(path).substr(begin, end - begin), L"AppData")) {
      return true;
    }
    if (separator == std::wstring::npos) {
      break;
    }
    begin = separator + 1U;
  }
  return false;
}

clonecore::Status verify_opened_path(
    const HANDLE handle,
    const std::wstring& expected,
    const std::wstring_view operation) {
  std::vector<wchar_t> actual(kMaximumWindowsPathCharacters, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle,
      actual.data(),
      static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0U ||
      static_cast<std::size_t>(length) >= actual.size()) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::identity_mismatch,
        operation,
        length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  if (!ordinal_equal_ignore_case(
          std::wstring_view(actual.data(), length), extended_path(expected))) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"opened handleの実体パスが固定パスと一致しません"));
  }
  return clonecore::success_status();
}

struct ObjectIdentity final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};
};

bool same_object(
    const ObjectIdentity& left,
    const ObjectIdentity& right) noexcept {
  return left.volume_serial == right.volume_serial &&
         left.file_id == right.file_id;
}

struct DirectoryObservation final {
  ObjectIdentity object{};
};

clonecore::Result<DirectoryObservation> observe_directory(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_ID_INFO identity{};
  if (GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileIdInfo, &identity, sizeof(identity)) == FALSE) {
    return clonecore::Result<DirectoryObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return support_failure<DirectoryObservation>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparseディレクトリではありません");
  }
  DirectoryObservation result{
      .object = {.volume_serial = identity.VolumeSerialNumber},
  };
  static_assert(sizeof(identity.FileId.Identifier) == 16U);
  std::memcpy(
      result.object.file_id.data(),
      identity.FileId.Identifier,
      result.object.file_id.size());
  return clonecore::Result<DirectoryObservation>::success(result);
}

struct PinnedDirectory final {
  clonecore::UniqueHandle handle;
  DirectoryObservation observation;
};

clonecore::Result<PinnedDirectory> open_directory(
    const std::wstring& path,
    const bool pin_against_rename,
    const std::wstring_view operation) {
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
      (pin_against_rename ? 0U : FILE_SHARE_DELETE);
  clonecore::UniqueHandle directory(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      share,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return clonecore::Result<PinnedDirectory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  auto observed = observe_directory(directory.get(), operation);
  if (!observed) {
    return clonecore::Result<PinnedDirectory>::failure(observed.error());
  }
  const auto path_matches = verify_opened_path(directory.get(), path, operation);
  if (!path_matches) {
    return clonecore::Result<PinnedDirectory>::failure(path_matches.error());
  }
  return clonecore::Result<PinnedDirectory>::success(PinnedDirectory{
      .handle = std::move(directory),
      .observation = observed.take_value(),
  });
}

clonecore::Status verify_directory_chain(
    const std::wstring& directory,
    const std::wstring_view operation) {
  std::size_t separator = directory.find(L'\\', 3U);
  while (separator != std::wstring::npos) {
    auto current = open_directory(
        directory.substr(0U, separator), false, operation);
    if (!current) {
      return clonecore::Status::failure(current.error());
    }
    separator = directory.find(L'\\', separator + 1U);
  }
  auto final = open_directory(directory, false, operation);
  return final ? clonecore::success_status()
               : clonecore::Status::failure(final.error());
}

struct FileObservation final {
  ObjectIdentity object{};
  std::uint64_t size_bytes{};
  std::uint64_t allocation_size{};
  std::uint64_t last_write_utc_100ns{};
  std::uint64_t change_time_utc_100ns{};
  std::uint32_t link_count{};
};

clonecore::Result<FileObservation> observe_regular_single_link_file(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_ID_INFO identity{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileIdInfo, &identity, sizeof(identity)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE)) != 0U ||
      standard.EndOfFile.QuadPart < 0 ||
      standard.AllocationSize.QuadPart < 0 ||
      basic.LastWriteTime.QuadPart < 0 || basic.ChangeTime.QuadPart < 0 ||
      standard.NumberOfLinks != 1U || standard.DeletePending != FALSE) {
    return support_failure<FileObservation>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparse単一linkファイルとして識別できません");
  }
  FileObservation result{
      .object = {.volume_serial = identity.VolumeSerialNumber},
      .size_bytes = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .allocation_size =
          static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
      .last_write_utc_100ns =
          static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
      .change_time_utc_100ns =
          static_cast<std::uint64_t>(basic.ChangeTime.QuadPart),
      .link_count = standard.NumberOfLinks,
  };
  std::memcpy(
      result.object.file_id.data(),
      identity.FileId.Identifier,
      result.object.file_id.size());
  return clonecore::Result<FileObservation>::success(result);
}

bool same_complete_file(
    const FileObservation& left,
    const FileObservation& right) noexcept {
  return same_object(left.object, right.object) &&
         left.size_bytes == right.size_bytes &&
         left.allocation_size == right.allocation_size &&
         left.last_write_utc_100ns == right.last_write_utc_100ns &&
         left.change_time_utc_100ns == right.change_time_utc_100ns &&
         left.link_count == right.link_count;
}

bool same_published_file(
    const FileObservation& before_rename,
    const FileObservation& after_rename) noexcept {
  return same_object(before_rename.object, after_rename.object) &&
         before_rename.size_bytes == after_rename.size_bytes &&
         before_rename.allocation_size == after_rename.allocation_size &&
         after_rename.link_count == 1U;
}

struct ConfiguredPaths final {
  std::wstring executable;
  std::wstring application_directory;
  std::wstring data_directory;
  std::wstring log_directory;
  std::wstring output_directory;
  std::wstring final_path;
  std::wstring partial_path;
};

clonecore::Result<ConfiguredPaths> configure_paths(
    const std::wstring& executable_path,
    const std::wstring& final_zip_path) {
  auto executable = canonical_local_path(
      executable_path, L"サポートZIP EXE path");
  if (!executable) {
    return clonecore::Result<ConfiguredPaths>::failure(executable.error());
  }
  auto final_path = canonical_local_path(
      final_zip_path, L"サポートZIP完成path");
  if (!final_path) {
    return clonecore::Result<ConfiguredPaths>::failure(final_path.error());
  }
  constexpr std::wstring_view kZipExtension{L".zip"};
  if (final_path.value().size() <= kZipExtension.size() ||
      !ordinal_equal_ignore_case(
          std::wstring_view(final_path.value()).substr(
              final_path.value().size() - kZipExtension.size()),
          kZipExtension)) {
    return support_failure<ConfiguredPaths>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"サポートZIP完成path",
        L"完成先は.zip拡張子のローカル絶対パスで指定してください");
  }
  if (contains_appdata_component(executable.value()) ||
      contains_appdata_component(final_path.value())) {
    return support_failure<ConfiguredPaths>(
        clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"サポートZIP AppData境界",
        L"EXE隣dataおよびサポートZIPにAppDataを使用できません");
  }
  auto application = parent_path(
      executable.value(), L"サポートZIP application path");
  if (!application) {
    return clonecore::Result<ConfiguredPaths>::failure(application.error());
  }
  auto data = child_path(
      application.value(), L"data", L"サポートZIP data path");
  if (!data) {
    return clonecore::Result<ConfiguredPaths>::failure(data.error());
  }
  auto logs = child_path(
      data.value(), L"logs", L"サポートZIP logs path");
  if (!logs) {
    return clonecore::Result<ConfiguredPaths>::failure(logs.error());
  }
  auto output = parent_path(
      final_path.value(), L"サポートZIP出力親path");
  if (!output) {
    return clonecore::Result<ConfiguredPaths>::failure(output.error());
  }
  if (final_path.value().size() + std::wstring_view(L".partial").size() >=
      kMaximumWindowsPathCharacters) {
    return support_failure<ConfiguredPaths>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"サポートZIP partial path",
        L"隣接partial名がWindowsパス上限を超えます");
  }
  auto partial = canonical_local_path(
      final_path.value() + L".partial", L"サポートZIP partial path");
  if (!partial) {
    return clonecore::Result<ConfiguredPaths>::failure(partial.error());
  }
  if (path_inside_or_equal(final_path.value(), data.value()) ||
      path_inside_or_equal(partial.value(), data.value())) {
    return support_failure<ConfiguredPaths>(
        clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"サポートZIP portable data境界",
        L"完成ZIPとpartialをEXE隣dataまたはその配下へ保存できません");
  }
  return clonecore::Result<ConfiguredPaths>::success(ConfiguredPaths{
      .executable = executable.take_value(),
      .application_directory = application.take_value(),
      .data_directory = data.take_value(),
      .log_directory = logs.take_value(),
      .output_directory = output.take_value(),
      .final_path = final_path.take_value(),
      .partial_path = partial.take_value(),
  });
}

clonecore::Result<FileObservation> observe_regular_path(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  auto observed = observe_regular_single_link_file(file.get(), operation);
  if (!observed) {
    return observed;
  }
  const auto path_matches = verify_opened_path(file.get(), path, operation);
  if (!path_matches) {
    return clonecore::Result<FileObservation>::failure(path_matches.error());
  }
  return observed;
}

clonecore::Status require_path_absent(
    const std::wstring& path,
    const std::wstring_view operation) {
  SetLastError(ERROR_SUCCESS);
  clonecore::UniqueHandle existing(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (existing) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_FILE_EXISTS,
        std::wstring(operation),
        L"既存ファイルまたはディレクトリは上書きしません"));
  }
  const DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed, operation, error));
  }
  return clonecore::success_status();
}

clonecore::Status seek_begin(
    const HANDLE handle,
    const std::wstring_view operation) {
  LARGE_INTEGER beginning{};
  if (SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed, operation, GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_bounded_file(
    const HANDLE handle,
    const std::uint64_t maximum_bytes,
    const std::wstring_view operation) {
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == FALSE) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  if (size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > maximum_bytes ||
      static_cast<std::uint64_t>(size.QuadPart) >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::size_t>::max)())) {
    return support_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
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
    DWORD read = 0U;
    if (ReadFile(
            handle,
            bytes.data() + consumed,
            amount,
            &read,
            nullptr) == FALSE ||
        read == 0U) {
      const DWORD error = GetLastError();
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              operation,
              error == ERROR_SUCCESS ? ERROR_HANDLE_EOF : error));
    }
    consumed += static_cast<std::size_t>(read);
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

clonecore::Result<std::wstring> decode_strict_utf8(
    const std::span<const std::byte> bytes) {
  if (bytes.size() >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return support_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"サポートZIPログUTF-8 decode",
        L"UTF-8本文がWindows変換上限を超えます");
  }
  if (bytes.empty()) {
    return clonecore::Result<std::wstring>::success({});
  }
  const int length = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<int>(bytes.size()),
      nullptr,
      0);
  if (length <= 0) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_data,
            L"サポートZIPログUTF-8 decode",
            GetLastError()));
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  const int converted = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      reinterpret_cast<const char*>(bytes.data()),
      static_cast<int>(bytes.size()),
      result.data(),
      length);
  if (converted != length || result.find(L'\0') != std::wstring::npos) {
    return support_failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        converted == length ? ERROR_INVALID_DATA : GetLastError(),
        L"サポートZIPログUTF-8 decode",
        L"UTF-8本文を完全かつNULなしで変換できません");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

clonecore::Result<std::vector<std::byte>> encode_strict_utf8(
    const std::wstring_view value) {
  if (value.size() >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return support_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"サポートZIP追加マスクUTF-8 encode",
        L"追加マスク結果がWindows変換上限を超えます");
  }
  if (value.empty()) {
    return clonecore::Result<std::vector<std::byte>>::success({});
  }
  const int length = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (length <= 0) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_data,
            L"サポートZIP追加マスクUTF-8 encode",
            GetLastError()));
  }
  std::vector<std::byte> result(static_cast<std::size_t>(length));
  const int converted = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      reinterpret_cast<char*>(result.data()),
      length,
      nullptr,
      nullptr);
  if (converted != length) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_data,
            L"サポートZIP追加マスクUTF-8 encode",
            GetLastError()));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(result));
}

bool append_with_archive_bound(
    std::vector<std::byte>& output,
    const std::span<const std::byte> value) {
  if (output.size() >
          static_cast<std::size_t>(kSupportZipMaximumArchiveBytes) ||
      value.size() >
      static_cast<std::size_t>(kSupportZipMaximumArchiveBytes) -
          output.size()) {
    return false;
  }
  output.insert(output.end(), value.begin(), value.end());
  return true;
}

clonecore::Result<std::vector<std::byte>> mask_log_bytes(
    const std::span<const std::byte> source) {
  if (source.size() < kUtf8Bom.size() ||
      !std::equal(kUtf8Bom.begin(), kUtf8Bom.end(), source.begin())) {
    return support_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"サポートZIP製品ログBOM確認",
        L"製品所有ログに必須のUTF-8 BOMがありません");
  }
  auto decoded = decode_strict_utf8(source.subspan(kUtf8Bom.size()));
  if (!decoded) {
    return clonecore::Result<std::vector<std::byte>>::failure(decoded.error());
  }
  std::vector<std::byte> masked;
  masked.reserve(source.size());
  masked.insert(masked.end(), kUtf8Bom.begin(), kUtf8Bom.end());
  std::size_t cursor = 0U;
  while (cursor < decoded.value().size()) {
    const std::size_t line_end =
        decoded.value().find_first_of(L"\r\n", cursor);
    const std::size_t content_end =
        line_end == std::wstring::npos ? decoded.value().size() : line_end;
    const std::wstring_view line(decoded.value().data() + cursor,
                                 content_end - cursor);
    std::wstring sanitized = clonecore::sanitize_main_log_message(line);
    if (!line.empty() && sanitized.empty()) {
      sanitized = L"[PRIVATE]";
    }
    auto encoded = encode_strict_utf8(sanitized);
    if (!encoded) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          encoded.error());
    }
    if (!append_with_archive_bound(masked, encoded.value())) {
      return support_failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP追加マスク",
          L"追加マスク後ログがarchive上限を超えます");
    }
    if (line_end == std::wstring::npos) {
      cursor = decoded.value().size();
      continue;
    }
    const wchar_t first = decoded.value()[line_end];
    masked.push_back(static_cast<std::byte>(first));
    cursor = line_end + 1U;
    if (first == L'\r' && cursor < decoded.value().size() &&
        decoded.value()[cursor] == L'\n') {
      masked.push_back(std::byte{0x0A});
      ++cursor;
    }
    if (masked.size() >
        static_cast<std::size_t>(kSupportZipMaximumArchiveBytes)) {
      return support_failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP追加マスク",
          L"追加マスク後ログがarchive上限を超えます");
    }
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(masked));
}

bool safe_archive_basename(const std::wstring& file_name) noexcept {
  if (!classify_product_log_file_name(file_name).has_value() ||
      file_name.empty() || file_name == L"." || file_name == L".." ||
      file_name.find_first_of(L"\\/:") != std::wstring::npos) {
    return false;
  }
  return std::all_of(
      file_name.begin(), file_name.end(), [](const wchar_t value) {
        return value >= 0x20 && value <= 0x7E;
      });
}

std::string basename_utf8(const std::wstring& file_name) {
  std::string result;
  result.reserve(file_name.size());
  for (const wchar_t value : file_name) {
    result.push_back(static_cast<char>(value));
  }
  return result;
}

struct PreparedLog final {
  std::wstring file_name;
  FileObservation source;
  std::vector<std::byte> masked;
  std::uint32_t crc32{};
};

clonecore::Result<PreparedLog> prepare_log(
    const std::wstring& log_directory,
    const std::wstring& file_name) {
  if (!safe_archive_basename(file_name)) {
    return support_failure<PreparedLog>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"サポートZIP entry名",
        L"製品所有basenameとして確認できません");
  }
  auto path = child_path(
      log_directory, file_name, L"サポートZIP製品ログpath");
  if (!path) {
    return clonecore::Result<PreparedLog>::failure(path.error());
  }
  clonecore::UniqueHandle file(CreateFileW(
      extended_path(path.value()).c_str(),
      GENERIC_READ | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    return clonecore::Result<PreparedLog>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"サポートZIP製品ログopen",
            GetLastError()));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"サポートZIP製品ログ初期再識別");
  if (!before) {
    return clonecore::Result<PreparedLog>::failure(before.error());
  }
  if (before.value().size_bytes > kSupportZipMaximumSourceFileBytes) {
    return support_failure<PreparedLog>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"サポートZIP製品ログ寸法",
        L"1ファイルのretention上限200 MiBを超えます");
  }
  const auto path_matches = verify_opened_path(
      file.get(), path.value(), L"サポートZIP製品ログ実体path");
  if (!path_matches) {
    return clonecore::Result<PreparedLog>::failure(path_matches.error());
  }
  auto bytes = read_bounded_file(
      file.get(),
      kSupportZipMaximumSourceFileBytes,
      L"サポートZIP製品ログ有界読取り");
  if (!bytes) {
    return clonecore::Result<PreparedLog>::failure(bytes.error());
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"サポートZIP製品ログ読取り後再識別");
  if (!after || !same_complete_file(before.value(), after.value())) {
    return after
        ? support_failure<PreparedLog>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"サポートZIP製品ログ読取り後再識別",
              L"File ID、寸法、時刻またはlink数が読取り中に変化しました")
        : clonecore::Result<PreparedLog>::failure(after.error());
  }
  auto masked = mask_log_bytes(bytes.value());
  if (!masked) {
    return clonecore::Result<PreparedLog>::failure(masked.error());
  }
  const std::uint32_t checksum = clonecore::crc32(masked.value());
  return clonecore::Result<PreparedLog>::success(PreparedLog{
      .file_name = file_name,
      .source = after.take_value(),
      .masked = masked.take_value(),
      .crc32 = checksum,
  });
}

class UniqueFindHandle final {
 public:
  explicit UniqueFindHandle(
      const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
      : handle_(handle) {}
  ~UniqueFindHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(FindClose(handle_));
    }
  }
  UniqueFindHandle(const UniqueFindHandle&) = delete;
  UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

clonecore::Result<std::vector<std::wstring>> enumerate_owned_log_names(
    const std::wstring& log_directory) {
  auto pattern = child_path(
      log_directory, L"*", L"サポートZIP製品ログ列挙pattern");
  if (!pattern) {
    return clonecore::Result<std::vector<std::wstring>>::failure(
        pattern.error());
  }
  WIN32_FIND_DATAW found{};
  UniqueFindHandle find(FindFirstFileW(
      extended_path(pattern.value()).c_str(), &found));
  if (!find.valid()) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return clonecore::Result<std::vector<std::wstring>>::success({});
    }
    return clonecore::Result<std::vector<std::wstring>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::enumeration_failed,
            L"サポートZIP製品ログ列挙",
            error));
  }
  std::vector<std::wstring> names;
  for (;;) {
    std::wstring file_name(found.cFileName);
    if (classify_product_log_file_name(file_name).has_value()) {
      if (names.size() >= kSupportZipMaximumCandidateCount) {
        return support_failure<std::vector<std::wstring>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_TOO_MANY_NAMES,
            L"サポートZIP製品ログ件数",
            L"製品ログ候補件数の安全上限4096件を超えます");
      }
      names.push_back(std::move(file_name));
    }
    if (FindNextFileW(find.get(), &found) == FALSE) {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_FILES) {
        return clonecore::Result<std::vector<std::wstring>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::enumeration_failed,
                L"サポートZIP製品ログ列挙",
                error));
      }
      break;
    }
  }
  std::sort(names.begin(), names.end(), ordinal_less_ignore_case);
  for (std::size_t index = 1U; index < names.size(); ++index) {
    if (ordinal_equal_ignore_case(names[index - 1U], names[index])) {
      return support_failure<std::vector<std::wstring>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"サポートZIP製品ログ列挙",
          L"大文字小文字だけが異なる重複製品ログ名があります");
    }
  }
  return clonecore::Result<std::vector<std::wstring>>::success(
      std::move(names));
}

struct CandidateObservation final {
  SupportZipSelectionCandidate selection;
  FileObservation source;
};

clonecore::Result<CandidateObservation> inspect_log_candidate(
    const std::wstring& log_directory,
    const std::wstring& file_name) {
  if (!safe_archive_basename(file_name)) {
    return support_failure<CandidateObservation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"サポートZIP候補basename",
        L"製品所有basenameとして確認できません");
  }
  auto path = child_path(
      log_directory, file_name, L"サポートZIP候補path");
  if (!path) {
    return clonecore::Result<CandidateObservation>::failure(path.error());
  }
  clonecore::UniqueHandle file(CreateFileW(
      extended_path(path.value()).c_str(),
      GENERIC_READ | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    return clonecore::Result<CandidateObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"サポートZIP候補open",
            GetLastError()));
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"サポートZIP候補初期再識別");
  if (!before) {
    return clonecore::Result<CandidateObservation>::failure(before.error());
  }
  const auto path_matches = verify_opened_path(
      file.get(), path.value(), L"サポートZIP候補実体path");
  if (!path_matches) {
    return clonecore::Result<CandidateObservation>::failure(
        path_matches.error());
  }
  if (before.value().size_bytes < kUtf8Bom.size()) {
    return support_failure<CandidateObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"サポートZIP候補BOM確認",
        L"製品所有ログに必須のUTF-8 BOMがありません");
  }
  std::array<std::byte, kUtf8Bom.size()> prefix{};
  DWORD read = 0U;
  if (ReadFile(
          file.get(),
          prefix.data(),
          static_cast<DWORD>(prefix.size()),
          &read,
          nullptr) == FALSE ||
      read != static_cast<DWORD>(prefix.size()) || prefix != kUtf8Bom) {
    return support_failure<CandidateObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"サポートZIP候補BOM確認",
        L"製品所有ログに必須のUTF-8 BOMがありません");
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"サポートZIP候補BOM後再識別");
  if (!after || !same_complete_file(before.value(), after.value())) {
    return after
        ? support_failure<CandidateObservation>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"サポートZIP候補BOM後再識別",
              L"候補のFile ID、寸法、時刻またはlink数が変化しました")
        : clonecore::Result<CandidateObservation>::failure(after.error());
  }
  return clonecore::Result<CandidateObservation>::success({
      .selection = {
          .file_name = file_name,
          .size_bytes = after.value().size_bytes,
          .last_write_utc_100ns = after.value().last_write_utc_100ns,
      },
      .source = after.take_value(),
  });
}

struct PreparedLogSet final {
  std::vector<PreparedLog> entries;
  std::size_t candidate_count{};
  std::size_t excluded_count{};
};

clonecore::Result<PreparedLogSet> prepare_all_logs(
    const std::wstring& log_directory) {
  auto names = enumerate_owned_log_names(log_directory);
  if (!names) {
    return clonecore::Result<PreparedLogSet>::failure(names.error());
  }
  if (names.value().empty()) {
    return support_failure<PreparedLogSet>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_NOT_FOUND,
        L"サポートZIP製品ログ計画",
        L"含有対象の製品所有ログがありません");
  }
  std::vector<CandidateObservation> candidates;
  candidates.reserve(names.value().size());
  std::vector<SupportZipSelectionCandidate> selection_input;
  selection_input.reserve(names.value().size());
  for (const auto& name : names.value()) {
    auto candidate = inspect_log_candidate(log_directory, name);
    if (!candidate) {
      return clonecore::Result<PreparedLogSet>::failure(candidate.error());
    }
    selection_input.push_back(candidate.value().selection);
    candidates.push_back(candidate.take_value());
  }
  auto selection = select_support_zip_candidates(selection_input);
  if (!selection) {
    return clonecore::Result<PreparedLogSet>::failure(selection.error());
  }
  std::vector<PreparedLog> prepared;
  prepared.reserve(selection.value().selected_indices.size());
  std::uint64_t source_total = 0U;
  std::uint64_t masked_total = 0U;
  for (const std::size_t selected_index :
       selection.value().selected_indices) {
    if (selected_index >= candidates.size()) {
      return support_failure<PreparedLogSet>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"サポートZIP subset index",
          L"選択結果が候補範囲外です");
    }
    const auto& candidate = candidates[selected_index];
    auto entry = prepare_log(
        log_directory, candidate.selection.file_name);
    if (!entry) {
      return clonecore::Result<PreparedLogSet>::failure(
          entry.error());
    }
    if (!same_complete_file(entry.value().source, candidate.source)) {
      return support_failure<PreparedLogSet>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"サポートZIP subset再識別",
          L"metadata選択後に製品ログのFile ID、寸法、時刻またはlink数が変化しました");
    }
    if (entry.value().source.size_bytes >
            kSupportZipMaximumSourceTotalBytes - source_total ||
        entry.value().masked.size() >
            static_cast<std::size_t>(kSupportZipMaximumArchiveBytes) -
                static_cast<std::size_t>(masked_total)) {
      return support_failure<PreparedLogSet>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP製品ログ合計寸法",
          L"入力200 MiBまたは追加マスク後256 MiBの安全上限を超えます");
    }
    source_total += entry.value().source.size_bytes;
    masked_total += entry.value().masked.size();
    prepared.push_back(entry.take_value());
  }
  return clonecore::Result<PreparedLogSet>::success({
      .entries = std::move(prepared),
      .candidate_count = candidates.size(),
      .excluded_count = selection.value().excluded_count,
  });
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xFFU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

clonecore::Result<std::vector<std::byte>> build_stored_zip(
    const std::vector<PreparedLog>& entries) {
  if (entries.empty() || entries.size() > kSupportZipMaximumEntryCount ||
      entries.size() >
          static_cast<std::size_t>(
              (std::numeric_limits<std::uint16_t>::max)())) {
    return support_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"サポートZIP構築",
        L"ZIP entry件数が有界形式の範囲外です");
  }
  std::uint64_t projected = kZipEndBytes;
  for (const auto& entry : entries) {
    const std::string name = basename_utf8(entry.file_name);
    if (name.empty() ||
        name.size() > static_cast<std::size_t>(
                          (std::numeric_limits<std::uint16_t>::max)()) ||
        entry.masked.size() > static_cast<std::size_t>(
                                  (std::numeric_limits<std::uint32_t>::max)())) {
      return support_failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP構築",
          L"entry名またはstored本文がZIP32上限を超えます");
    }
    const std::uint64_t increment = kZipLocalFixedBytes + name.size() +
        entry.masked.size() + kZipCentralFixedBytes + name.size();
    if (increment > kSupportZipMaximumArchiveBytes - projected) {
      return support_failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP構築",
          L"完成ZIPが256 MiBの安全上限を超えます");
    }
    projected += increment;
  }
  if (projected >
      static_cast<std::uint64_t>(
          (std::numeric_limits<std::uint32_t>::max)())) {
    return support_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"サポートZIP構築",
        L"ZIP64を必要とするarchiveは作成しません");
  }

  std::vector<std::byte> archive;
  archive.reserve(static_cast<std::size_t>(projected));
  std::vector<std::uint32_t> local_offsets;
  local_offsets.reserve(entries.size());
  for (const auto& entry : entries) {
    const std::string name = basename_utf8(entry.file_name);
    local_offsets.push_back(static_cast<std::uint32_t>(archive.size()));
    append_u32(archive, kZipLocalHeaderSignature);
    append_u16(archive, kZipVersion20);
    append_u16(archive, kZipUtf8Flag);
    append_u16(archive, kZipStoredMethod);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, entry.crc32);
    append_u32(archive, static_cast<std::uint32_t>(entry.masked.size()));
    append_u32(archive, static_cast<std::uint32_t>(entry.masked.size()));
    append_u16(archive, static_cast<std::uint16_t>(name.size()));
    append_u16(archive, 0U);
    archive.insert(
        archive.end(),
        reinterpret_cast<const std::byte*>(name.data()),
        reinterpret_cast<const std::byte*>(name.data() + name.size()));
    archive.insert(
        archive.end(), entry.masked.begin(), entry.masked.end());
  }

  const std::uint32_t central_offset =
      static_cast<std::uint32_t>(archive.size());
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    const std::string name = basename_utf8(entry.file_name);
    append_u32(archive, kZipCentralHeaderSignature);
    append_u16(archive, kZipVersion20);
    append_u16(archive, kZipVersion20);
    append_u16(archive, kZipUtf8Flag);
    append_u16(archive, kZipStoredMethod);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, entry.crc32);
    append_u32(archive, static_cast<std::uint32_t>(entry.masked.size()));
    append_u32(archive, static_cast<std::uint32_t>(entry.masked.size()));
    append_u16(archive, static_cast<std::uint16_t>(name.size()));
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, local_offsets[index]);
    archive.insert(
        archive.end(),
        reinterpret_cast<const std::byte*>(name.data()),
        reinterpret_cast<const std::byte*>(name.data() + name.size()));
  }
  const std::uint32_t central_size =
      static_cast<std::uint32_t>(archive.size()) - central_offset;
  append_u32(archive, kZipEndSignature);
  append_u16(archive, 0U);
  append_u16(archive, 0U);
  append_u16(archive, static_cast<std::uint16_t>(entries.size()));
  append_u16(archive, static_cast<std::uint16_t>(entries.size()));
  append_u32(archive, central_size);
  append_u32(archive, central_offset);
  append_u16(archive, 0U);
  if (archive.size() != static_cast<std::size_t>(projected)) {
    return support_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"サポートZIP構築寸法",
        L"事前計算と完成archive寸法が一致しません");
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(archive));
}

bool read_u16_at(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    std::uint16_t& value) noexcept {
  if (offset > bytes.size() || 2U > bytes.size() - offset) {
    return false;
  }
  value = static_cast<std::uint16_t>(
      std::to_integer<std::uint16_t>(bytes[offset]) |
      (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
  return true;
}

bool read_u32_at(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    std::uint32_t& value) noexcept {
  if (offset > bytes.size() || 4U > bytes.size() - offset) {
    return false;
  }
  value = 0U;
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    value |= std::to_integer<std::uint32_t>(
                 bytes[offset + (shift / 8U)])
             << shift;
  }
  return true;
}

bool span_matches_string(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::string& expected) noexcept {
  if (offset > bytes.size() || expected.size() > bytes.size() - offset) {
    return false;
  }
  return std::equal(
      expected.begin(),
      expected.end(),
      bytes.begin() + static_cast<std::ptrdiff_t>(offset),
      [](const char left, const std::byte right) {
        return static_cast<unsigned char>(left) ==
               std::to_integer<unsigned char>(right);
      });
}

clonecore::Status verify_stored_zip(
    const std::span<const std::byte> archive,
    const std::vector<PreparedLog>& expected) {
  if (archive.size() < kZipEndBytes ||
      archive.size() >
          static_cast<std::size_t>(kSupportZipMaximumArchiveBytes) ||
      expected.empty() || expected.size() > kSupportZipMaximumEntryCount) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"サポートZIP完全検証",
        L"archive寸法または期待entry件数が不正です"));
  }
  const std::size_t end_offset = archive.size() - kZipEndBytes;
  std::uint32_t signature{};
  std::uint16_t disk{};
  std::uint16_t central_disk{};
  std::uint16_t disk_count{};
  std::uint16_t total_count{};
  std::uint32_t central_size{};
  std::uint32_t central_offset{};
  std::uint16_t comment_length{};
  if (!read_u32_at(archive, end_offset, signature) ||
      signature != kZipEndSignature ||
      !read_u16_at(archive, end_offset + 4U, disk) || disk != 0U ||
      !read_u16_at(archive, end_offset + 6U, central_disk) ||
      central_disk != 0U ||
      !read_u16_at(archive, end_offset + 8U, disk_count) ||
      !read_u16_at(archive, end_offset + 10U, total_count) ||
      disk_count != expected.size() || total_count != expected.size() ||
      !read_u32_at(archive, end_offset + 12U, central_size) ||
      !read_u32_at(archive, end_offset + 16U, central_offset) ||
      !read_u16_at(archive, end_offset + 20U, comment_length) ||
      comment_length != 0U ||
      static_cast<std::size_t>(central_offset) > end_offset ||
      static_cast<std::size_t>(central_size) !=
          end_offset - static_cast<std::size_t>(central_offset)) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"サポートZIP EOCD検証",
        L"単一disk・commentなしEOCDまたはcentral範囲が一致しません"));
  }

  std::size_t central_cursor = central_offset;
  std::size_t local_cursor = 0U;
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    const auto& entry = expected[index];
    const std::string name = basename_utf8(entry.file_name);
    std::uint16_t made_by{};
    std::uint16_t needed{};
    std::uint16_t flags{};
    std::uint16_t method{};
    std::uint16_t modified_time{};
    std::uint16_t modified_date{};
    std::uint32_t crc{};
    std::uint32_t compressed{};
    std::uint32_t uncompressed{};
    std::uint16_t name_length{};
    std::uint16_t extra_length{};
    std::uint16_t entry_comment{};
    std::uint16_t entry_disk{};
    std::uint16_t internal_attributes{};
    std::uint32_t external_attributes{};
    std::uint32_t local_offset{};
    if (!read_u32_at(archive, central_cursor, signature) ||
        signature != kZipCentralHeaderSignature ||
        !read_u16_at(archive, central_cursor + 4U, made_by) ||
        made_by != kZipVersion20 ||
        !read_u16_at(archive, central_cursor + 6U, needed) ||
        needed != kZipVersion20 ||
        !read_u16_at(archive, central_cursor + 8U, flags) ||
        flags != kZipUtf8Flag ||
        !read_u16_at(archive, central_cursor + 10U, method) ||
        method != kZipStoredMethod ||
        !read_u16_at(archive, central_cursor + 12U, modified_time) ||
        modified_time != 0U ||
        !read_u16_at(archive, central_cursor + 14U, modified_date) ||
        modified_date != 0U ||
        !read_u32_at(archive, central_cursor + 16U, crc) ||
        crc != entry.crc32 ||
        !read_u32_at(archive, central_cursor + 20U, compressed) ||
        compressed != entry.masked.size() ||
        !read_u32_at(archive, central_cursor + 24U, uncompressed) ||
        uncompressed != entry.masked.size() ||
        !read_u16_at(archive, central_cursor + 28U, name_length) ||
        name_length != name.size() ||
        !read_u16_at(archive, central_cursor + 30U, extra_length) ||
        extra_length != 0U ||
        !read_u16_at(archive, central_cursor + 32U, entry_comment) ||
        entry_comment != 0U ||
        !read_u16_at(archive, central_cursor + 34U, entry_disk) ||
        entry_disk != 0U ||
        !read_u16_at(
            archive, central_cursor + 36U, internal_attributes) ||
        internal_attributes != 0U ||
        !read_u32_at(
            archive, central_cursor + 38U, external_attributes) ||
        external_attributes != 0U ||
        !read_u32_at(archive, central_cursor + 42U, local_offset) ||
        local_offset != local_cursor ||
        !span_matches_string(
            archive, central_cursor + kZipCentralFixedBytes, name)) {
      return clonecore::Status::failure(support_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"サポートZIP central entry検証",
          L"central header、basename、CRC、寸法またはlocal offsetが一致しません"));
    }
    central_cursor += kZipCentralFixedBytes + name.size();

    std::uint16_t local_needed{};
    std::uint16_t local_flags{};
    std::uint16_t local_method{};
    std::uint16_t local_time{};
    std::uint16_t local_date{};
    std::uint32_t local_crc{};
    std::uint32_t local_compressed{};
    std::uint32_t local_uncompressed{};
    std::uint16_t local_name_length{};
    std::uint16_t local_extra_length{};
    if (!read_u32_at(archive, local_cursor, signature) ||
        signature != kZipLocalHeaderSignature ||
        !read_u16_at(archive, local_cursor + 4U, local_needed) ||
        local_needed != kZipVersion20 ||
        !read_u16_at(archive, local_cursor + 6U, local_flags) ||
        local_flags != kZipUtf8Flag ||
        !read_u16_at(archive, local_cursor + 8U, local_method) ||
        local_method != kZipStoredMethod ||
        !read_u16_at(archive, local_cursor + 10U, local_time) ||
        local_time != 0U ||
        !read_u16_at(archive, local_cursor + 12U, local_date) ||
        local_date != 0U ||
        !read_u32_at(archive, local_cursor + 14U, local_crc) ||
        local_crc != entry.crc32 ||
        !read_u32_at(archive, local_cursor + 18U, local_compressed) ||
        local_compressed != entry.masked.size() ||
        !read_u32_at(archive, local_cursor + 22U, local_uncompressed) ||
        local_uncompressed != entry.masked.size() ||
        !read_u16_at(archive, local_cursor + 26U, local_name_length) ||
        local_name_length != name.size() ||
        !read_u16_at(archive, local_cursor + 28U, local_extra_length) ||
        local_extra_length != 0U ||
        !span_matches_string(
            archive, local_cursor + kZipLocalFixedBytes, name)) {
      return clonecore::Status::failure(support_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"サポートZIP local entry検証",
          L"local header、basename、CRCまたは寸法が一致しません"));
    }
    const std::size_t data_offset =
        local_cursor + kZipLocalFixedBytes + name.size();
    if (data_offset > archive.size() ||
        entry.masked.size() > archive.size() - data_offset ||
        data_offset + entry.masked.size() > central_offset ||
        !std::equal(
            entry.masked.begin(),
            entry.masked.end(),
            archive.begin() + static_cast<std::ptrdiff_t>(data_offset)) ||
        clonecore::crc32(archive.subspan(data_offset, entry.masked.size())) !=
            entry.crc32) {
      return clonecore::Status::failure(support_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"サポートZIP stored本文検証",
          L"stored本文の全byteまたはCRC-32が一致しません"));
    }
    local_cursor = data_offset + entry.masked.size();
  }
  if (local_cursor != central_offset || central_cursor != end_offset) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"サポートZIP完全範囲検証",
        L"local dataまたはcentral directoryに隙間・余分なbyteがあります"));
  }
  return clonecore::success_status();
}

clonecore::Status write_all(
    const HANDLE handle,
    const std::span<const std::byte> bytes) {
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>((std::min)(
        bytes.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written = 0U;
    if (WriteFile(
            handle,
            bytes.data() + consumed,
            amount,
            &written,
            nullptr) == FALSE ||
        written == 0U) {
      const DWORD error = GetLastError();
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"サポートZIP partial書込み",
          error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error));
    }
    consumed += static_cast<std::size_t>(written);
  }
  return clonecore::success_status();
}

clonecore::Status mark_delete_pending(const HANDLE handle) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition)) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"サポートZIP所有partial cleanup",
        GetLastError()));
  }
  return clonecore::success_status();
}

// A create-new partial is owned by this call. Any exception after creation
// must therefore clean that exact open object by handle, never by pathname.
class OwnedPartialCleanup final {
 public:
  explicit OwnedPartialCleanup(clonecore::UniqueHandle& handle) noexcept
      : handle_(&handle) {}
  ~OwnedPartialCleanup() noexcept {
    if (active_ && handle_ != nullptr && *handle_) {
      static_cast<void>(mark_delete_pending(handle_->get()));
      handle_->reset();
    }
  }

  OwnedPartialCleanup(const OwnedPartialCleanup&) = delete;
  OwnedPartialCleanup& operator=(const OwnedPartialCleanup&) = delete;

  void release() noexcept { active_ = false; }

 private:
  clonecore::UniqueHandle* handle_{};
  bool active_{true};
};

clonecore::Status rename_no_replace(
    const HANDLE handle,
    const std::wstring& final_path) {
  if (final_path.size() >
      (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t)) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"サポートZIP非上書きpublish",
        L"完成pathがWindows rename上限を超えます"));
  }
  const std::size_t name_bytes = final_path.size() * sizeof(wchar_t);
  const std::size_t fixed_bytes = offsetof(FILE_RENAME_INFO, FileName);
  if (name_bytes > (std::numeric_limits<DWORD>::max)() ||
      fixed_bytes > (std::numeric_limits<DWORD>::max)() - name_bytes ||
      fixed_bytes + name_bytes >
          (std::numeric_limits<DWORD>::max)() - sizeof(wchar_t)) {
    return clonecore::Status::failure(support_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"サポートZIP非上書きpublish",
        L"完成pathがWindows rename上限を超えます"));
  }
  std::vector<std::byte> buffer(
      fixed_bytes + name_bytes + sizeof(wchar_t), std::byte{0});
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(name_bytes);
  std::memcpy(rename->FileName, final_path.data(), name_bytes);
  if (SetFileInformationByHandle(
          handle,
          FileRenameInfo,
          buffer.data(),
          static_cast<DWORD>(buffer.size())) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"サポートZIP非上書きatomic publish",
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<SupportZipCreationReport> fail_owned_partial(
    clonecore::UniqueHandle& partial,
    const clonecore::Error& primary) {
  const auto cleanup = mark_delete_pending(partial.get());
  partial.reset();
  if (!cleanup) {
    return clonecore::Result<SupportZipCreationReport>::failure(support_error(
        clonecore::ErrorCode::io_failed,
        cleanup.error().native_code,
        L"サポートZIP失敗後cleanup",
        primary.message + L" / " + cleanup.error().message));
  }
  return clonecore::Result<SupportZipCreationReport>::failure(primary);
}

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
              L"サポートZIP current EXE path",
              GetLastError()));
    }
    if (static_cast<std::size_t>(copied) < buffer.size()) {
      return clonecore::Result<std::wstring>::success(
          std::wstring(buffer.data(), copied));
    }
    if (buffer.size() >= kMaximumWindowsPathCharacters) {
      return support_failure<std::wstring>(
          clonecore::ErrorCode::query_failed,
          ERROR_INSUFFICIENT_BUFFER,
          L"サポートZIP current EXE path",
          L"現在のEXE完全pathを有界bufferで取得できません");
    }
    buffer.resize(
        (std::min)(buffer.size() * 2U, kMaximumWindowsPathCharacters), L'\0');
  }
}

}  // namespace

clonecore::Result<SupportZipCandidateSelection>
select_support_zip_candidates(
    const std::vector<SupportZipSelectionCandidate>& candidates) noexcept {
  try {
    if (candidates.empty()) {
      return support_failure<SupportZipCandidateSelection>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_NOT_FOUND,
          L"サポートZIP newest-first選択",
          L"含有候補の製品所有ログがありません");
    }
    if (candidates.size() > kSupportZipMaximumCandidateCount) {
      return support_failure<SupportZipCandidateSelection>(
          clonecore::ErrorCode::invalid_data,
          ERROR_TOO_MANY_NAMES,
          L"サポートZIP newest-first選択",
          L"製品ログ候補件数が4096件の安全上限を超えます");
    }
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t index = 0U; index < order.size(); ++index) {
      order[index] = index;
      if (!safe_archive_basename(candidates[index].file_name) ||
          candidates[index].size_bytes < kUtf8Bom.size()) {
        return support_failure<SupportZipCandidateSelection>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"サポートZIP newest-first候補",
            L"製品所有basenameまたはBOMを含む最小寸法を確認できません");
      }
    }
    std::sort(
        order.begin(),
        order.end(),
        [&candidates](const std::size_t left, const std::size_t right) {
          const auto& left_value = candidates[left];
          const auto& right_value = candidates[right];
          if (left_value.last_write_utc_100ns !=
              right_value.last_write_utc_100ns) {
            return left_value.last_write_utc_100ns >
                   right_value.last_write_utc_100ns;
          }
          return ordinal_less_ignore_case(
              left_value.file_name, right_value.file_name);
        });
    for (std::size_t index = 1U; index < order.size(); ++index) {
      if (ordinal_equal_ignore_case(
              candidates[order[index - 1U]].file_name,
              candidates[order[index]].file_name)) {
        return support_failure<SupportZipCandidateSelection>(
            clonecore::ErrorCode::invalid_data,
            ERROR_DUP_NAME,
            L"サポートZIP newest-first候補",
            L"大文字小文字だけが異なる重複製品ログ名があります");
      }
    }

    SupportZipCandidateSelection selection;
    selection.selected_indices.reserve(
        (std::min)(order.size(), kSupportZipMaximumEntryCount));
    for (const std::size_t index : order) {
      const std::uint64_t size = candidates[index].size_bytes;
      if (size > kSupportZipMaximumSourceFileBytes) {
        continue;
      }
      if (selection.selected_indices.empty() &&
          size > kSupportZipPreferredSubsetBytes) {
        selection.selected_indices.push_back(index);
        selection.selected_bytes = size;
        break;
      }
      if (selection.selected_indices.size() >=
              kSupportZipMaximumEntryCount ||
          selection.selected_bytes > kSupportZipPreferredSubsetBytes ||
          size > kSupportZipPreferredSubsetBytes -
                     selection.selected_bytes) {
        continue;
      }
      selection.selected_indices.push_back(index);
      selection.selected_bytes += size;
    }
    if (selection.selected_indices.empty()) {
      return support_failure<SupportZipCandidateSelection>(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP newest-first選択",
          L"retention上限200 MiB以内の製品ログを1件も選択できません");
    }
    if (selection.selected_bytes > kSupportZipMaximumSourceTotalBytes) {
      return support_failure<SupportZipCandidateSelection>(
          clonecore::ErrorCode::internal_error,
          ERROR_FILE_TOO_LARGE,
          L"サポートZIP newest-first選択",
          L"選択結果が200 MiBの安全上限を超えました");
    }
    selection.excluded_count =
        candidates.size() - selection.selected_indices.size();
    return clonecore::Result<SupportZipCandidateSelection>::success(
        std::move(selection));
  } catch (const std::bad_alloc&) {
    return support_failure<SupportZipCandidateSelection>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"サポートZIP newest-first選択",
        L"有界選択に必要なメモリを確保できません");
  } catch (...) {
    return support_failure<SupportZipCandidateSelection>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"サポートZIP newest-first選択",
        L"newest-first subsetを安全に選択できません");
  }
}

clonecore::Result<SupportZipPlan> SupportZipPlanBuilder::build(
    const std::wstring& executable_path,
    const std::wstring& final_zip_path) {
  auto paths = configure_paths(executable_path, final_zip_path);
  if (!paths) {
    return clonecore::Result<SupportZipPlan>::failure(paths.error());
  }
  const auto application_chain = verify_directory_chain(
      paths.value().application_directory,
      L"サポートZIP application chain");
  const auto data_chain = application_chain
      ? verify_directory_chain(
            paths.value().data_directory, L"サポートZIP data chain")
      : application_chain;
  const auto logs_chain = data_chain
      ? verify_directory_chain(
            paths.value().log_directory, L"サポートZIP logs chain")
      : data_chain;
  const auto output_chain = logs_chain
      ? verify_directory_chain(
            paths.value().output_directory, L"サポートZIP output chain")
      : logs_chain;
  if (!output_chain) {
    return clonecore::Result<SupportZipPlan>::failure(output_chain.error());
  }
  auto application = open_directory(
      paths.value().application_directory,
      true,
      L"サポートZIP application固定");
  auto data = application
      ? open_directory(
            paths.value().data_directory, true, L"サポートZIP data固定")
      : clonecore::Result<PinnedDirectory>::failure(application.error());
  auto logs = data
      ? open_directory(
            paths.value().log_directory, true, L"サポートZIP logs固定")
      : clonecore::Result<PinnedDirectory>::failure(data.error());
  auto output = logs
      ? open_directory(
            paths.value().output_directory,
            true,
            L"サポートZIP output固定")
      : clonecore::Result<PinnedDirectory>::failure(logs.error());
  if (!output) {
    return clonecore::Result<SupportZipPlan>::failure(output.error());
  }
  auto executable = observe_regular_path(
      paths.value().executable, L"サポートZIP EXE再識別");
  if (!executable) {
    return clonecore::Result<SupportZipPlan>::failure(executable.error());
  }
  const auto final_absent = require_path_absent(
      paths.value().final_path, L"サポートZIP完成先非上書き確認");
  const auto partial_absent = final_absent
      ? require_path_absent(
            paths.value().partial_path,
            L"サポートZIP隣接partial非上書き確認")
      : final_absent;
  if (!partial_absent) {
    return clonecore::Result<SupportZipPlan>::failure(partial_absent.error());
  }
  auto prepared = prepare_all_logs(paths.value().log_directory);
  if (!prepared) {
    return clonecore::Result<SupportZipPlan>::failure(prepared.error());
  }

  auto application_after = observe_directory(
      application.value().handle.get(), L"サポートZIP application再識別");
  auto data_after = observe_directory(
      data.value().handle.get(), L"サポートZIP data再識別");
  auto logs_after = observe_directory(
      logs.value().handle.get(), L"サポートZIP logs再識別");
  auto output_after = observe_directory(
      output.value().handle.get(), L"サポートZIP output再識別");
  if (!application_after || !data_after || !logs_after || !output_after ||
      !same_object(
          application.value().observation.object,
          application_after.value().object) ||
      !same_object(data.value().observation.object, data_after.value().object) ||
      !same_object(logs.value().observation.object, logs_after.value().object) ||
      !same_object(
          output.value().observation.object, output_after.value().object)) {
    const clonecore::Error error = !application_after
        ? application_after.error()
        : !data_after
            ? data_after.error()
            : !logs_after
                ? logs_after.error()
                : !output_after
                    ? output_after.error()
                    : support_error(
                          clonecore::ErrorCode::identity_mismatch,
                          ERROR_FILE_INVALID,
                          L"サポートZIP directory再識別",
                          L"計画中にdirectory File IDまたはvolumeが変化しました");
    return clonecore::Result<SupportZipPlan>::failure(error);
  }

  SupportZipPlan plan;
  plan.executable_path_ = paths.value().executable;
  plan.application_directory_ = paths.value().application_directory;
  plan.data_directory_ = paths.value().data_directory;
  plan.log_directory_ = paths.value().log_directory;
  plan.final_path_ = paths.value().final_path;
  plan.partial_path_ = paths.value().partial_path;
  const auto copy_object = [](const ObjectIdentity& source,
                              SupportZipPlan::ObjectIdentity& target) {
    target.volume_serial = source.volume_serial;
    target.file_id = source.file_id;
  };
  copy_object(executable.value().object, plan.executable_identity_);
  copy_object(
      application_after.value().object,
      plan.application_directory_identity_);
  copy_object(data_after.value().object, plan.data_directory_identity_);
  copy_object(logs_after.value().object, plan.log_directory_identity_);
  copy_object(output_after.value().object, plan.output_directory_identity_);
  plan.display_entries_.reserve(prepared.value().entries.size());
  plan.source_identities_.reserve(prepared.value().entries.size());
  for (const auto& entry : prepared.value().entries) {
    plan.display_entries_.push_back(SupportZipDisplayEntry{
        .archive_entry_name = entry.file_name,
        .source_size_bytes = entry.source.size_bytes,
        .masked_size_bytes = entry.masked.size(),
    });
    SupportZipPlan::SourceIdentity source{};
    copy_object(entry.source.object, source.object);
    source.size_bytes = entry.source.size_bytes;
    source.allocation_size = entry.source.allocation_size;
    source.last_write_utc_100ns = entry.source.last_write_utc_100ns;
    source.change_time_utc_100ns = entry.source.change_time_utc_100ns;
    source.link_count = entry.source.link_count;
    source.masked_crc32 = entry.crc32;
    plan.source_identities_.push_back(source);
    plan.source_total_bytes_ += entry.source.size_bytes;
    plan.masked_total_bytes_ += entry.masked.size();
  }
  plan.candidate_log_count_ = prepared.value().candidate_count;
  plan.excluded_log_count_ = prepared.value().excluded_count;
  return clonecore::Result<SupportZipPlan>::success(std::move(plan));
}

clonecore::Result<SupportZipPlan> plan_windows_support_zip(
    const std::wstring& executable_path,
    const std::wstring& final_zip_path) {
  try {
    return SupportZipPlanBuilder::build(executable_path, final_zip_path);
  } catch (const std::bad_alloc&) {
    return support_failure<SupportZipPlan>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"サポートZIP計画",
        L"有界計画に必要なメモリを確保できません");
  } catch (...) {
    return support_failure<SupportZipPlan>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"サポートZIP計画",
        L"サポートZIP計画を安全に構成できません");
  }
}

clonecore::Result<SupportZipPlan> plan_current_executable_support_zip(
    const std::wstring& final_zip_path) {
  try {
    auto executable = current_executable_path();
    if (!executable) {
      return clonecore::Result<SupportZipPlan>::failure(executable.error());
    }
    return SupportZipPlanBuilder::build(executable.value(), final_zip_path);
  } catch (const std::bad_alloc&) {
    return support_failure<SupportZipPlan>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"サポートZIP current EXE計画",
        L"有界計画に必要なメモリを確保できません");
  } catch (...) {
    return support_failure<SupportZipPlan>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"サポートZIP current EXE計画",
        L"current EXE用計画を安全に構成できません");
  }
}

clonecore::Result<SupportZipCreationReport> create_windows_support_zip(
    const SupportZipPlan& plan) {
  try {
    auto paths = configure_paths(plan.executable_path_, plan.final_path_);
    if (!paths) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          paths.error());
    }
    if (paths.value().application_directory != plan.application_directory_ ||
        paths.value().data_directory != plan.data_directory_ ||
        paths.value().log_directory != plan.log_directory_ ||
        paths.value().final_path != plan.final_path_ ||
        paths.value().partial_path != plan.partial_path_ ||
        plan.display_entries_.empty() ||
        plan.display_entries_.size() != plan.source_identities_.size() ||
        plan.display_entries_.size() > kSupportZipMaximumEntryCount) {
      return support_failure<SupportZipCreationReport>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"サポートZIP計画再構成",
          L"計画path、entry件数または内部bindingが一致しません");
    }
    const auto application_chain = verify_directory_chain(
        paths.value().application_directory,
        L"サポートZIP作成application chain");
    const auto data_chain = application_chain
        ? verify_directory_chain(
              paths.value().data_directory,
              L"サポートZIP作成data chain")
        : application_chain;
    const auto logs_chain = data_chain
        ? verify_directory_chain(
              paths.value().log_directory,
              L"サポートZIP作成logs chain")
        : data_chain;
    const auto output_chain = logs_chain
        ? verify_directory_chain(
              paths.value().output_directory,
              L"サポートZIP作成output chain")
        : logs_chain;
    if (!output_chain) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          output_chain.error());
    }
    auto application = open_directory(
        paths.value().application_directory,
        true,
        L"サポートZIP作成application固定");
    auto data = application
        ? open_directory(
              paths.value().data_directory,
              true,
              L"サポートZIP作成data固定")
        : clonecore::Result<PinnedDirectory>::failure(application.error());
    auto logs = data
        ? open_directory(
              paths.value().log_directory,
              true,
              L"サポートZIP作成logs固定")
        : clonecore::Result<PinnedDirectory>::failure(data.error());
    auto output = logs
        ? open_directory(
              paths.value().output_directory,
              true,
              L"サポートZIP作成output固定")
        : clonecore::Result<PinnedDirectory>::failure(logs.error());
    if (!output) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          output.error());
    }
    const auto plan_object_matches = [](
        const ObjectIdentity& actual,
        const SupportZipPlan::ObjectIdentity& expected) {
      return actual.volume_serial == expected.volume_serial &&
             actual.file_id == expected.file_id;
    };
    auto executable = observe_regular_path(
        paths.value().executable, L"サポートZIP作成EXE再識別");
    if (!executable ||
        !plan_object_matches(
            executable.value().object, plan.executable_identity_) ||
        !plan_object_matches(
            application.value().observation.object,
            plan.application_directory_identity_) ||
        !plan_object_matches(
            data.value().observation.object,
            plan.data_directory_identity_) ||
        !plan_object_matches(
            logs.value().observation.object,
            plan.log_directory_identity_) ||
        !plan_object_matches(
            output.value().observation.object,
            plan.output_directory_identity_)) {
      return executable
          ? support_failure<SupportZipCreationReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"サポートZIP計画directory/EXE再識別",
                L"計画後にEXE、data、logsまたは出力親のFile ID/volumeが変化しました")
          : clonecore::Result<SupportZipCreationReport>::failure(
                executable.error());
    }

    auto prepared = prepare_all_logs(paths.value().log_directory);
    if (!prepared) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          prepared.error());
    }
    std::uint64_t source_total = 0U;
    std::uint64_t masked_total = 0U;
    if (prepared.value().entries.size() != plan.display_entries_.size() ||
        prepared.value().candidate_count != plan.candidate_log_count_ ||
        prepared.value().excluded_count != plan.excluded_log_count_) {
      return support_failure<SupportZipCreationReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"サポートZIP計画entry再識別",
          L"利用者確認後に製品ログ件数が変化しました");
    }
    for (std::size_t index = 0U;
         index < prepared.value().entries.size();
         ++index) {
      const auto& actual = prepared.value().entries[index];
      const auto& display = plan.display_entries_[index];
      const auto& expected = plan.source_identities_[index];
      if (actual.file_name != display.archive_entry_name ||
          actual.source.object.volume_serial !=
              expected.object.volume_serial ||
          actual.source.object.file_id != expected.object.file_id ||
          actual.source.size_bytes != expected.size_bytes ||
          actual.source.allocation_size != expected.allocation_size ||
          actual.source.last_write_utc_100ns !=
              expected.last_write_utc_100ns ||
          actual.source.change_time_utc_100ns !=
              expected.change_time_utc_100ns ||
          actual.source.link_count != expected.link_count ||
          actual.source.size_bytes != display.source_size_bytes ||
          actual.masked.size() != display.masked_size_bytes ||
          actual.crc32 != expected.masked_crc32) {
        return support_failure<SupportZipCreationReport>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"サポートZIP計画entry再識別",
            L"利用者確認後にログのbasename、File ID、寸法、時刻、link数または追加マスク結果が変化しました");
      }
      source_total += actual.source.size_bytes;
      masked_total += actual.masked.size();
    }
    if (source_total != plan.source_total_bytes_ ||
        masked_total != plan.masked_total_bytes_) {
      return support_failure<SupportZipCreationReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"サポートZIP計画合計再識別",
          L"利用者確認済みの入力または追加マスク後合計寸法が変化しました");
    }
    auto archive = build_stored_zip(prepared.value().entries);
    if (!archive) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          archive.error());
    }
    const auto in_memory_verified =
        verify_stored_zip(archive.value(), prepared.value().entries);
    if (!in_memory_verified) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          in_memory_verified.error());
    }

    clonecore::UniqueHandle partial(CreateFileW(
        extended_path(paths.value().partial_path).c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!partial) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"サポートZIP隣接partial非上書き作成",
              GetLastError()));
    }
    OwnedPartialCleanup partial_cleanup(partial);
    const auto partial_path_matches = verify_opened_path(
        partial.get(),
        paths.value().partial_path,
        L"サポートZIP所有partial実体path");
    if (!partial_path_matches) {
      return fail_owned_partial(partial, partial_path_matches.error());
    }
    auto initial = observe_regular_single_link_file(
        partial.get(), L"サポートZIP所有partial初期再識別");
    if (!initial || initial.value().size_bytes != 0U ||
        initial.value().object.volume_serial !=
            plan.output_directory_identity_.volume_serial) {
      const clonecore::Error error = initial
          ? support_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"サポートZIP所有partial初期再識別",
                L"新規partialが空の単一linkまたは計画済み出力volumeではありません")
          : initial.error();
      return fail_owned_partial(partial, error);
    }
    const auto written = write_all(partial.get(), archive.value());
    if (!written) {
      return fail_owned_partial(partial, written.error());
    }
    if (FlushFileBuffers(partial.get()) == FALSE) {
      return fail_owned_partial(
          partial,
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"サポートZIP所有partial flush",
              GetLastError()));
    }
    auto readback = read_bounded_file(
        partial.get(),
        kSupportZipMaximumArchiveBytes,
        L"サポートZIP所有partial同一handle読戻し");
    if (!readback) {
      return fail_owned_partial(partial, readback.error());
    }
    auto completed = observe_regular_single_link_file(
        partial.get(), L"サポートZIP所有partial書込み後再識別");
    if (!completed || !same_object(initial.value().object,
                                   completed.value().object) ||
        completed.value().size_bytes != archive.value().size() ||
        completed.value().link_count != 1U ||
        readback.value() != archive.value()) {
      const clonecore::Error error = completed
          ? support_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"サポートZIP所有partial同一handle読戻し",
                L"File ID、寸法、link数または全archive byteが一致しません")
          : completed.error();
      return fail_owned_partial(partial, error);
    }
    const auto archive_verified =
        verify_stored_zip(readback.value(), prepared.value().entries);
    if (!archive_verified) {
      return fail_owned_partial(partial, archive_verified.error());
    }
    auto output_before_publish = observe_directory(
        output.value().handle.get(),
        L"サポートZIPpublish直前output再識別");
    if (!output_before_publish ||
        !plan_object_matches(
            output_before_publish.value().object,
            plan.output_directory_identity_)) {
      const clonecore::Error error = output_before_publish
          ? support_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"サポートZIPpublish直前output再識別",
                L"出力親directoryのFile IDまたはvolumeが変化しました")
          : output_before_publish.error();
      return fail_owned_partial(partial, error);
    }
    const auto published = rename_no_replace(partial.get(), plan.final_path_);
    if (!published) {
      return fail_owned_partial(partial, published.error());
    }
    partial_cleanup.release();
    auto final_observed = observe_regular_path(
        plan.final_path_, L"サポートZIP完成path/File ID再識別");
    if (!final_observed ||
        !same_published_file(completed.value(), final_observed.value())) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          !final_observed
              ? final_observed.error()
              : support_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_CRC,
                    L"サポートZIP完成後再識別",
                    L"完成pathがpublish済みFile ID、寸法、allocationまたはlink数と一致しません"));
    }
    if (FlushFileBuffers(partial.get()) == FALSE) {
      return clonecore::Result<SupportZipCreationReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"サポートZIP完成後同一handle flush",
              GetLastError()));
    }
    partial.reset();
    return clonecore::Result<SupportZipCreationReport>::success({
        .final_path = plan.final_path_,
        .entries = plan.display_entries_,
        .archive_size_bytes = archive.value().size(),
        .excluded_log_count = plan.excluded_log_count_,
        .local_only = true,
    });
  } catch (const std::bad_alloc&) {
    return support_failure<SupportZipCreationReport>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"サポートZIP作成",
        L"有界archive作成に必要なメモリを確保できません");
  } catch (...) {
    return support_failure<SupportZipCreationReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"サポートZIP作成",
        L"サポートZIP作成を安全に完了できません");
  }
}

}  // namespace ytec::windowsapp

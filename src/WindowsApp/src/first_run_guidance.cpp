#include "ytec/windowsapp/first_run_guidance.h"

#include "first_run_guidance_internal.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/windowsapp/startup_data_policy.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kMaximumWindowsPathCharacters = 32U * 1024U;
constexpr std::size_t kMaximumSettingsAllocationBytes = 1024U * 1024U;
constexpr std::array<std::byte, 8U> kFirstRunGuidanceMagic{
    std::byte{0x59}, std::byte{0x54}, std::byte{0x45}, std::byte{0x43},
    std::byte{0x55}, std::byte{0x49}, std::byte{0x47}, std::byte{0x00}};
constexpr std::size_t kSchemaOffset = 8U;
constexpr std::size_t kAcknowledgedOffset = 12U;
constexpr std::wstring_view kStageSuffix{L".partial"};
constexpr std::wstring_view kBackupSuffix{L".backup"};

clonecore::Error guidance_error(
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
clonecore::Result<T> guidance_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(guidance_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

FirstRunGuidanceInspection unavailable_inspection(
    const clonecore::Error& error) {
  return FirstRunGuidanceInspection{
      .state = FirstRunGuidanceDocumentState::storage_unavailable,
      .decision = plan_first_run_guidance(
          FirstRunGuidanceDocumentState::storage_unavailable),
      .diagnostic = error.operation + L": " + error.message,
      .native_code = error.native_code,
  };
}

FirstRunGuidanceInspection inspection_for_state(
    const FirstRunGuidanceDocumentState state,
    std::wstring diagnostic = {},
    const DWORD native_code = ERROR_SUCCESS) {
  return FirstRunGuidanceInspection{
      .state = state,
      .decision = plan_first_run_guidance(state),
      .diagnostic = std::move(diagnostic),
      .native_code = native_code,
  };
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
    return guidance_failure<std::wstring>(
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
        path.data() + component_begin,
        component_end - component_begin);
    if (component.empty() || component == L"." || component == L".." ||
        component.ends_with(L" ") || component.ends_with(L".")) {
      return guidance_failure<std::wstring>(
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
  wchar_t* file_part{};
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
    return guidance_failure<std::wstring>(
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
    return guidance_failure<std::wstring>(
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
    return guidance_failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        std::wstring(operation),
        L"固定設定pathがWindows上限を超えます");
  }
  return clonecore::Result<std::wstring>::success(
      parent + (root ? L"" : L"\\") + std::wstring(child));
}

bool contains_appdata_component(const std::wstring& path) noexcept {
  std::size_t begin = 3U;
  while (begin <= path.size()) {
    const std::size_t separator = path.find(L'\\', begin);
    const std::size_t end =
        separator == std::wstring::npos ? path.size() : separator;
    if (ordinal_equal_ignore_case(
            std::wstring_view(path).substr(begin, end - begin),
            L"AppData")) {
      return true;
    }
    if (separator == std::wstring::npos) {
      break;
    }
    begin = separator + 1U;
  }
  return false;
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
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
          std::wstring_view(actual.data(), length),
          extended_path(expected))) {
    return clonecore::Status::failure(guidance_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"opened handleの実体pathが固定pathと一致しません"));
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
          handle,
          FileIdInfo,
          &identity,
          sizeof(identity)) == FALSE) {
    return clonecore::Result<DirectoryObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return guidance_failure<DirectoryObservation>(
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
  std::wstring path;
  clonecore::UniqueHandle handle;
  DirectoryObservation observation;
};

clonecore::Result<PinnedDirectory> open_pinned_directory(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle directory(CreateFileW(
      extended_path(path).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return clonecore::Result<PinnedDirectory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  const auto path_matches = verify_opened_path(
      directory.get(), path, operation);
  if (!path_matches) {
    return clonecore::Result<PinnedDirectory>::failure(
        path_matches.error());
  }
  auto observation = observe_directory(directory.get(), operation);
  if (!observation) {
    return clonecore::Result<PinnedDirectory>::failure(
        observation.error());
  }
  return clonecore::Result<PinnedDirectory>::success(PinnedDirectory{
      .path = path,
      .handle = std::move(directory),
      .observation = observation.take_value(),
  });
}

struct ConfiguredPaths final {
  std::wstring application_directory;
  std::wstring data_directory;
  std::wstring final_path;
  std::wstring stage_path;
  std::wstring backup_path;
};

clonecore::Result<ConfiguredPaths> configure_paths(
    const std::wstring& data_directory) {
  auto canonical = canonical_local_path(
      data_directory, L"初回安全ガイドdata path");
  if (!canonical) {
    return clonecore::Result<ConfiguredPaths>::failure(canonical.error());
  }
  if (contains_appdata_component(canonical.value())) {
    return guidance_failure<ConfiguredPaths>(
        clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"初回安全ガイドAppData境界",
        L"AppDataまたはその配下へ設定を保存しません");
  }
  auto application = parent_path(
      canonical.value(), L"初回安全ガイドEXE親path");
  if (!application) {
    return clonecore::Result<ConfiguredPaths>::failure(application.error());
  }
  const std::size_t separator = canonical.value().find_last_of(L'\\');
  const std::wstring_view name = separator == std::wstring::npos
      ? std::wstring_view{}
      : std::wstring_view(canonical.value()).substr(separator + 1U);
  if (!ordinal_equal_ignore_case(name, L"data")) {
    return guidance_failure<ConfiguredPaths>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"初回安全ガイドdata境界",
        L"固定名dataディレクトリ以外は使用できません");
  }
  auto final_path = child_path(
      canonical.value(),
      kFirstRunGuidanceSettingsFileName,
      L"初回安全ガイド固定設定path");
  if (!final_path) {
    return clonecore::Result<ConfiguredPaths>::failure(final_path.error());
  }
  if (final_path.value().size() + kStageSuffix.size() >=
          kMaximumWindowsPathCharacters ||
      final_path.value().size() + kBackupSuffix.size() >=
          kMaximumWindowsPathCharacters) {
    return guidance_failure<ConfiguredPaths>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"初回安全ガイド固定設定path",
        L"固定stageまたはbackup pathがWindows上限を超えます");
  }
  return clonecore::Result<ConfiguredPaths>::success(ConfiguredPaths{
      .application_directory = application.take_value(),
      .data_directory = canonical.take_value(),
      .final_path = final_path.value(),
      .stage_path = final_path.value() + std::wstring(kStageSuffix),
      .backup_path = final_path.value() + std::wstring(kBackupSuffix),
  });
}

struct BoundDirectories final {
  ConfiguredPaths paths;
  std::vector<PinnedDirectory> chain;
};

clonecore::Result<BoundDirectories> bind_directory_chain(
    ConfiguredPaths paths) {
  std::vector<std::wstring> chain_paths;
  std::size_t separator = paths.data_directory.find(L'\\', 3U);
  while (separator != std::wstring::npos) {
    chain_paths.push_back(paths.data_directory.substr(0U, separator));
    separator = paths.data_directory.find(L'\\', separator + 1U);
  }
  chain_paths.push_back(paths.data_directory);

  std::vector<PinnedDirectory> chain;
  chain.reserve(chain_paths.size());
  for (const auto& path : chain_paths) {
    auto directory = open_pinned_directory(
        path, L"初回安全ガイドdata全ancestor固定");
    if (!directory) {
      return clonecore::Result<BoundDirectories>::failure(
          directory.error());
    }
    chain.push_back(directory.take_value());
  }
  if (chain.empty()) {
    return guidance_failure<BoundDirectories>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"初回安全ガイドdata全ancestor固定",
        L"dataディレクトリの固定対象がありません");
  }
  const auto application = std::find_if(
      chain.begin(), chain.end(), [&](const PinnedDirectory& directory) {
        return ordinal_equal_ignore_case(
            directory.path, paths.application_directory);
      });
  if (application == chain.end() ||
      application->observation.object.volume_serial !=
          chain.back().observation.object.volume_serial) {
    return guidance_failure<BoundDirectories>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_SAME_DEVICE,
        L"初回安全ガイドEXE/data固定",
        L"EXE親とdataを同じ固定volume上で識別できません");
  }
  return clonecore::Result<BoundDirectories>::success(BoundDirectories{
      .paths = std::move(paths),
      .chain = std::move(chain),
  });
}

clonecore::Status reidentify_directory_chain(
    const BoundDirectories& bound,
    const std::wstring_view operation) {
  for (const auto& directory : bound.chain) {
    auto observed = observe_directory(directory.handle.get(), operation);
    if (!observed ||
        !same_object(
            directory.observation.object,
            observed.value().object)) {
      return observed
          ? clonecore::Status::failure(guidance_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                std::wstring(operation),
                L"ancestor directoryのFile IDまたはvolumeが変化しました"))
          : clonecore::Status::failure(observed.error());
    }
  }
  return clonecore::success_status();
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
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_READONLY)) != 0U ||
      standard.EndOfFile.QuadPart < 0 ||
      standard.AllocationSize.QuadPart < 0 ||
      basic.LastWriteTime.QuadPart < 0 || basic.ChangeTime.QuadPart < 0 ||
      standard.NumberOfLinks != 1U || standard.DeletePending != FALSE) {
    return guidance_failure<FileObservation>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の書込み可能な非reparse単一linkファイルとして識別できません");
  }
  FileObservation result{
      .object = {.volume_serial = identity.VolumeSerialNumber},
      .size_bytes = static_cast<std::uint64_t>(
          standard.EndOfFile.QuadPart),
      .allocation_size = static_cast<std::uint64_t>(
          standard.AllocationSize.QuadPart),
      .last_write_utc_100ns = static_cast<std::uint64_t>(
          basic.LastWriteTime.QuadPart),
      .change_time_utc_100ns = static_cast<std::uint64_t>(
          basic.ChangeTime.QuadPart),
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
    const FileObservation& staged,
    const FileObservation& published) noexcept {
  return same_object(staged.object, published.object) &&
         staged.size_bytes == published.size_bytes &&
         staged.allocation_size == published.allocation_size &&
         published.link_count == 1U;
}

bool same_renamed_file(
    const FileObservation& before,
    const FileObservation& after) noexcept {
  return same_object(before.object, after.object) &&
         before.size_bytes == after.size_bytes &&
         before.allocation_size == after.allocation_size &&
         before.link_count == after.link_count;
}

clonecore::Result<std::vector<std::byte>> read_bounded_file(
    const HANDLE handle,
    const std::uint64_t size,
    const std::wstring_view operation) {
  if (size > kFirstRunGuidanceMaximumDocumentBytes ||
      size > static_cast<std::uint64_t>(
                 (std::numeric_limits<std::size_t>::max)())) {
    return guidance_failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        std::wstring(operation),
        L"固定設定ファイルが上限を超えます");
  }
  LARGE_INTEGER beginning{};
  if (SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == FALSE) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    DWORD read{};
    const DWORD requested = static_cast<DWORD>(bytes.size() - consumed);
    if (ReadFile(
            handle,
            bytes.data() + consumed,
            requested,
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

struct LoadedState final {
  FirstRunGuidanceInspection inspection;
  std::optional<FileObservation> file;
  std::vector<std::byte> bytes;
  clonecore::UniqueHandle handle;
};

LoadedState load_bound_state(const BoundDirectories& bound) {
  clonecore::UniqueHandle file(CreateFileW(
      extended_path(bound.paths.final_path).c_str(),
      GENERIC_READ | DELETE,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return LoadedState{
          .inspection = inspection_for_state(
              FirstRunGuidanceDocumentState::missing),
      };
    }
    return LoadedState{
        .inspection = unavailable_inspection(clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"初回安全ガイド固定設定open",
            error)),
    };
  }
  const auto path_matches = verify_opened_path(
      file.get(),
      bound.paths.final_path,
      L"初回安全ガイド固定設定実体path");
  if (!path_matches) {
    return LoadedState{
        .inspection = unavailable_inspection(path_matches.error()),
    };
  }
  auto before = observe_regular_single_link_file(
      file.get(), L"初回安全ガイド固定設定属性");
  if (!before) {
    return LoadedState{
        .inspection = unavailable_inspection(before.error()),
    };
  }
  if (before.value().size_bytes >
          kFirstRunGuidanceMaximumDocumentBytes ||
      before.value().allocation_size > kMaximumSettingsAllocationBytes) {
    return LoadedState{
        .inspection = inspection_for_state(
            FirstRunGuidanceDocumentState::malformed,
            L"固定設定ファイルの寸法がschema v1の上限外です",
            ERROR_FILE_TOO_LARGE),
        .file = before.take_value(),
    };
  }
  auto bytes = read_bounded_file(
      file.get(),
      before.value().size_bytes,
      L"初回安全ガイド固定設定同一handle読取り");
  if (!bytes) {
    return LoadedState{
        .inspection = unavailable_inspection(bytes.error()),
    };
  }
  auto after = observe_regular_single_link_file(
      file.get(), L"初回安全ガイド固定設定読取り後再識別");
  if (!after || !same_complete_file(before.value(), after.value())) {
    const clonecore::Error error = after
        ? guidance_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"初回安全ガイド固定設定読取り後再識別",
              L"読取り中にFile ID、寸法、link数または時刻が変化しました")
        : after.error();
    return LoadedState{
        .inspection = unavailable_inspection(error),
    };
  }
  const auto state = classify_first_run_guidance_document(bytes.value());
  std::wstring diagnostic;
  DWORD native_code = ERROR_SUCCESS;
  if (state == FirstRunGuidanceDocumentState::malformed) {
    diagnostic =
        L"既存設定はschema v1として完全一致せず、変更せず保持しました";
    native_code = ERROR_INVALID_DATA;
  } else if (state == FirstRunGuidanceDocumentState::newer_schema) {
    diagnostic =
        L"この版より新しいschemaの設定を検出し、変更せず保持しました";
    native_code = ERROR_REVISION_MISMATCH;
  }
  return LoadedState{
      .inspection = inspection_for_state(
          state, std::move(diagnostic), native_code),
      .file = after.take_value(),
      .bytes = bytes.take_value(),
      .handle = std::move(file),
  };
}

clonecore::Status write_all(
    const HANDLE handle,
    const std::span<const std::byte> bytes) {
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    DWORD written{};
    const DWORD requested = static_cast<DWORD>(bytes.size() - consumed);
    if (WriteFile(
            handle,
            bytes.data() + consumed,
            requested,
            &written,
            nullptr) == FALSE ||
        written == 0U) {
      const DWORD error = GetLastError();
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"初回安全ガイド所有stage書込み",
          error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error));
    }
    consumed += static_cast<std::size_t>(written);
  }
  return clonecore::success_status();
}

clonecore::Status mark_delete_pending(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition)) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        operation,
        GetLastError()));
  }
  return clonecore::success_status();
}

class OwnedPathCleanup final {
 public:
  OwnedPathCleanup(
      clonecore::UniqueHandle& handle,
      std::wstring expected_path) noexcept
      : handle_(&handle), expected_path_(std::move(expected_path)) {}

  ~OwnedPathCleanup() noexcept {
    if (!active_ || handle_ == nullptr || !*handle_) {
      return;
    }
    const auto still_owned_path = verify_opened_path(
        handle_->get(), expected_path_, L"初回安全ガイド所有一時file cleanup");
    if (still_owned_path) {
      static_cast<void>(mark_delete_pending(
          handle_->get(), L"初回安全ガイド所有一時file cleanup"));
    }
    handle_->reset();
  }

  OwnedPathCleanup(const OwnedPathCleanup&) = delete;
  OwnedPathCleanup& operator=(const OwnedPathCleanup&) = delete;

  void release() noexcept { active_ = false; }

 private:
  clonecore::UniqueHandle* handle_{};
  std::wstring expected_path_;
  bool active_{true};
};

clonecore::Status rename_no_replace(
    const HANDLE handle,
    const std::wstring& final_path) {
  if (final_path.size() >
      (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t)) {
    return clonecore::Status::failure(guidance_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"初回安全ガイド非上書きpublish",
        L"固定設定pathがWindows rename上限を超えます"));
  }
  const std::size_t name_bytes = final_path.size() * sizeof(wchar_t);
  const std::size_t fixed_bytes = offsetof(FILE_RENAME_INFO, FileName);
  if (name_bytes > (std::numeric_limits<DWORD>::max)() ||
      fixed_bytes > (std::numeric_limits<DWORD>::max)() - name_bytes ||
      fixed_bytes + name_bytes >
          (std::numeric_limits<DWORD>::max)() - sizeof(wchar_t)) {
    return clonecore::Status::failure(guidance_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"初回安全ガイド非上書きpublish",
        L"固定設定pathがWindows rename buffer上限を超えます"));
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
        L"初回安全ガイド非上書きatomic publish",
        GetLastError()));
  }
  return clonecore::success_status();
}

bool same_backing_observation(
    const StartupDataBackingObservation& left,
    const StartupDataBackingObservation& right) noexcept {
  return left.disk_number == right.disk_number &&
         left.data_directory_exists == right.data_directory_exists &&
         ordinal_equal_ignore_case(
             left.application_directory, right.application_directory) &&
         ordinal_equal_ignore_case(
             left.data_directory, right.data_directory);
}

clonecore::Result<StartupDataBackingObservation>
current_existing_data_observation() {
  auto observation = inspect_windows_startup_data_backing();
  if (!observation) {
    return clonecore::Result<StartupDataBackingObservation>::failure(
        observation.error());
  }
  if (!observation.value().data_directory_exists) {
    return guidance_failure<StartupDataBackingObservation>(
        clonecore::ErrorCode::access_denied,
        ERROR_PATH_NOT_FOUND,
        L"初回安全ガイドcurrent EXE隣data確認",
        L"dataディレクトリが存在しないため作成せず、設定を保存しません");
  }
  return observation;
}

}  // namespace

std::array<std::byte, kFirstRunGuidanceDocumentBytes>
serialize_first_run_guidance_document(const bool acknowledged) noexcept {
  std::array<std::byte, kFirstRunGuidanceDocumentBytes> bytes{};
  std::copy(kFirstRunGuidanceMagic.begin(), kFirstRunGuidanceMagic.end(),
            bytes.begin());
  const std::uint32_t schema = kFirstRunGuidanceSchemaVersion;
  bytes[kSchemaOffset] = static_cast<std::byte>(schema & 0xFFU);
  bytes[kSchemaOffset + 1U] =
      static_cast<std::byte>((schema >> 8U) & 0xFFU);
  bytes[kSchemaOffset + 2U] =
      static_cast<std::byte>((schema >> 16U) & 0xFFU);
  bytes[kSchemaOffset + 3U] =
      static_cast<std::byte>((schema >> 24U) & 0xFFU);
  bytes[kAcknowledgedOffset] = acknowledged
      ? std::byte{0x01}
      : std::byte{0x00};
  return bytes;
}

FirstRunGuidanceDocumentState classify_first_run_guidance_document(
    const std::span<const std::byte> bytes) noexcept {
  if (bytes.size() < kSchemaOffset + sizeof(std::uint32_t) ||
      !std::equal(
          kFirstRunGuidanceMagic.begin(),
          kFirstRunGuidanceMagic.end(),
          bytes.begin())) {
    return FirstRunGuidanceDocumentState::malformed;
  }
  const std::uint32_t schema =
      std::to_integer<std::uint32_t>(bytes[kSchemaOffset]) |
      (std::to_integer<std::uint32_t>(bytes[kSchemaOffset + 1U]) << 8U) |
      (std::to_integer<std::uint32_t>(bytes[kSchemaOffset + 2U]) << 16U) |
      (std::to_integer<std::uint32_t>(bytes[kSchemaOffset + 3U]) << 24U);
  if (schema > kFirstRunGuidanceSchemaVersion) {
    return FirstRunGuidanceDocumentState::newer_schema;
  }
  if (schema != kFirstRunGuidanceSchemaVersion ||
      bytes.size() != kFirstRunGuidanceDocumentBytes ||
      (bytes[kAcknowledgedOffset] != std::byte{0x00} &&
       bytes[kAcknowledgedOffset] != std::byte{0x01}) ||
      bytes[kAcknowledgedOffset + 1U] != std::byte{0x00} ||
      bytes[kAcknowledgedOffset + 2U] != std::byte{0x00} ||
      bytes[kAcknowledgedOffset + 3U] != std::byte{0x00}) {
    return FirstRunGuidanceDocumentState::malformed;
  }
  return bytes[kAcknowledgedOffset] == std::byte{0x01}
      ? FirstRunGuidanceDocumentState::acknowledged
      : FirstRunGuidanceDocumentState::acknowledgement_pending;
}

FirstRunGuidanceDecision plan_first_run_guidance(
    const FirstRunGuidanceDocumentState state) noexcept {
  switch (state) {
    case FirstRunGuidanceDocumentState::missing:
    case FirstRunGuidanceDocumentState::acknowledgement_pending:
      return FirstRunGuidanceDecision{
          .show_guidance = true,
          .acknowledgement_may_be_saved = true,
      };
    case FirstRunGuidanceDocumentState::acknowledged:
      return FirstRunGuidanceDecision{
          .show_guidance = false,
          .acknowledgement_may_be_saved = false,
      };
    case FirstRunGuidanceDocumentState::malformed:
    case FirstRunGuidanceDocumentState::newer_schema:
    case FirstRunGuidanceDocumentState::storage_unavailable:
      return FirstRunGuidanceDecision{
          .show_guidance = true,
          .acknowledgement_may_be_saved = false,
      };
  }
  return FirstRunGuidanceDecision{
      .show_guidance = true,
      .acknowledgement_may_be_saved = false,
  };
}

FirstRunGuidanceDiagnosticButtonLayout
calculate_first_run_guidance_diagnostic_button_layout(
    const int secondary_left,
    const int secondary_right,
    const int gap) noexcept {
  if (secondary_right <= secondary_left || gap < 0 ||
      secondary_right - secondary_left <= gap) {
    return {};
  }
  const int available = secondary_right - secondary_left - gap;
  const int update_width = available / 2;
  const int guidance_width = available - update_width;
  return FirstRunGuidanceDiagnosticButtonLayout{
      .update_left = secondary_left,
      .update_width = update_width,
      .guidance_left = secondary_left + update_width + gap,
      .guidance_width = guidance_width,
  };
}

namespace detail {

FirstRunGuidanceInspection
inspect_first_run_guidance_in_existing_data_directory(
    const std::wstring& data_directory) noexcept {
  try {
    auto paths = configure_paths(data_directory);
    if (!paths) {
      return unavailable_inspection(paths.error());
    }
    auto bound = bind_directory_chain(paths.take_value());
    if (!bound) {
      return unavailable_inspection(bound.error());
    }
    const auto before = reidentify_directory_chain(
        bound.value(), L"初回安全ガイド読取り前ancestor再識別");
    if (!before) {
      return unavailable_inspection(before.error());
    }
    auto loaded = load_bound_state(bound.value());
    const auto after = reidentify_directory_chain(
        bound.value(), L"初回安全ガイド読取り後ancestor再識別");
    if (!after) {
      return unavailable_inspection(after.error());
    }
    return loaded.inspection;
  } catch (const std::bad_alloc&) {
    return unavailable_inspection(guidance_error(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"初回安全ガイド設定読取り",
        L"有界設定読取りに必要なメモリを確保できません"));
  } catch (...) {
    return unavailable_inspection(guidance_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"初回安全ガイド設定読取り",
        L"設定を安全に判定できません"));
  }
}

clonecore::Result<FirstRunGuidanceSaveReport>
save_first_run_guidance_in_existing_data_directory(
    const std::wstring& data_directory,
    const FirstRunGuidanceBeforeReplaceHook before_replace,
    void* const hook_context) noexcept {
  try {
    auto paths = configure_paths(data_directory);
    if (!paths) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          paths.error());
    }
    auto bound = bind_directory_chain(paths.take_value());
    if (!bound) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          bound.error());
    }
    auto current = load_bound_state(bound.value());
    if (current.inspection.state ==
        FirstRunGuidanceDocumentState::acknowledged) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::success({
          .disposition =
              FirstRunGuidanceSaveDisposition::already_acknowledged,
          .final_path = bound.value().paths.final_path,
      });
    }
    if (!current.inspection.decision.acknowledgement_may_be_saved) {
      return guidance_failure<FirstRunGuidanceSaveReport>(
          clonecore::ErrorCode::access_denied,
          current.inspection.native_code == ERROR_SUCCESS
              ? ERROR_INVALID_DATA
              : current.inspection.native_code,
          L"初回安全ガイド設定保存前確認",
          current.inspection.diagnostic.empty()
              ? L"既存設定を安全に更新できないため保持します"
              : current.inspection.diagnostic);
    }
    const bool replacing = current.inspection.state ==
        FirstRunGuidanceDocumentState::acknowledgement_pending;
    if (replacing && !current.file.has_value()) {
      return guidance_failure<FirstRunGuidanceSaveReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"初回安全ガイド設定置換前確認",
          L"置換対象のFile IDを確認できません");
    }

    clonecore::UniqueHandle stage(CreateFileW(
        extended_path(bound.value().paths.stage_path).c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!stage) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"初回安全ガイド所有stage CREATE_NEW",
              GetLastError()));
    }
    OwnedPathCleanup stage_cleanup(
        stage, bound.value().paths.stage_path);
    const auto stage_path_matches = verify_opened_path(
        stage.get(),
        bound.value().paths.stage_path,
        L"初回安全ガイド所有stage実体path");
    if (!stage_path_matches) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          stage_path_matches.error());
    }
    auto stage_initial = observe_regular_single_link_file(
        stage.get(), L"初回安全ガイド所有stage初期属性");
    if (!stage_initial || stage_initial.value().size_bytes != 0U ||
        stage_initial.value().object.volume_serial !=
            bound.value().chain.back().observation.object.volume_serial) {
      return stage_initial
          ? guidance_failure<FirstRunGuidanceSaveReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"初回安全ガイド所有stage初期再識別",
                L"新規stageが空の単一linkまたはdataと同じvolumeではありません")
          : clonecore::Result<FirstRunGuidanceSaveReport>::failure(
                stage_initial.error());
    }

    const auto document =
        serialize_first_run_guidance_document(true);
    const auto written = write_all(stage.get(), document);
    if (!written) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          written.error());
    }
    if (FlushFileBuffers(stage.get()) == FALSE) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"初回安全ガイド所有stage flush",
              GetLastError()));
    }
    auto readback = read_bounded_file(
        stage.get(),
        document.size(),
        L"初回安全ガイド所有stage同一handle読戻し");
    if (!readback ||
        !std::equal(
            document.begin(), document.end(), readback.value().begin())) {
      return readback
          ? guidance_failure<FirstRunGuidanceSaveReport>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"初回安全ガイド所有stage同一handle読戻し",
                L"書込み済みschema v1全byteが一致しません")
          : clonecore::Result<FirstRunGuidanceSaveReport>::failure(
                readback.error());
    }
    auto stage_complete = observe_regular_single_link_file(
        stage.get(), L"初回安全ガイド所有stage書込み後属性");
    if (!stage_complete ||
        !same_object(
            stage_initial.value().object,
            stage_complete.value().object) ||
        stage_complete.value().size_bytes != document.size() ||
        stage_complete.value().allocation_size >
            kMaximumSettingsAllocationBytes) {
      return stage_complete
          ? guidance_failure<FirstRunGuidanceSaveReport>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"初回安全ガイド所有stage書込み後再識別",
                L"File ID、寸法、allocationまたはlink数が一致しません")
          : clonecore::Result<FirstRunGuidanceSaveReport>::failure(
                stage_complete.error());
    }

    const auto directory_before_publish = reidentify_directory_chain(
        bound.value(), L"初回安全ガイドpublish直前ancestor再識別");
    if (!directory_before_publish) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          directory_before_publish.error());
    }
    auto last = load_bound_state(bound.value());
    const bool final_unchanged = replacing
        ? last.inspection.state ==
                FirstRunGuidanceDocumentState::acknowledgement_pending &&
            last.file.has_value() &&
            same_complete_file(current.file.value(), last.file.value()) &&
            last.bytes == current.bytes
        : last.inspection.state == FirstRunGuidanceDocumentState::missing;
    if (!final_unchanged) {
      return guidance_failure<FirstRunGuidanceSaveReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"初回安全ガイドpublish直前固定設定再識別",
          L"固定設定が確認後に出現または変化したため上書きしません");
    }
    last.handle.reset();

    if (replacing) {
      if (!current.handle) {
        return guidance_failure<FirstRunGuidanceSaveReport>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_HANDLE,
            L"初回安全ガイド既存設定handle確認",
            L"置換対象を同一handleで保持できません");
      }
      const auto backed_up = rename_no_replace(
          current.handle.get(), bound.value().paths.backup_path);
      if (!backed_up) {
        return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
            backed_up.error());
      }
      const auto backup_path_matches = verify_opened_path(
          current.handle.get(),
          bound.value().paths.backup_path,
          L"初回安全ガイドrecovery backup実体path");
      auto observed = observe_regular_single_link_file(
          current.handle.get(),
          L"初回安全ガイドrecovery backup属性");
      if (!backup_path_matches || !observed ||
          !same_renamed_file(current.file.value(), observed.value())) {
        const clonecore::Error verification_error =
            !backup_path_matches
            ? backup_path_matches.error()
            : !observed
                  ? observed.error()
                  : guidance_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_FILE_INVALID,
                        L"初回安全ガイドrecovery backup再識別",
                        L"移動後の元設定File IDまたは属性が一致しません");
        const auto restored = rename_no_replace(
            current.handle.get(), bound.value().paths.final_path);
        if (!restored) {
          return guidance_failure<FirstRunGuidanceSaveReport>(
              clonecore::ErrorCode::identity_mismatch,
              restored.error().native_code,
              L"初回安全ガイドbackup再識別失敗後の復元",
              L"元設定を固定 .backup に保持しました");
        }
        return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
            verification_error);
      }
      if (before_replace != nullptr) {
        before_replace(hook_context);
      }
      const auto published = rename_no_replace(
          stage.get(), bound.value().paths.final_path);
      if (!published) {
        const auto restored = rename_no_replace(
            current.handle.get(), bound.value().paths.final_path);
        if (!restored) {
          return guidance_failure<FirstRunGuidanceSaveReport>(
              clonecore::ErrorCode::io_failed,
              restored.error().native_code,
              L"初回安全ガイドpublish失敗後の復元",
              L"新設定を公開できず、元設定を固定 .backup に保持しました");
        }
        return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
            published.error());
      }
    } else {
      const auto published = rename_no_replace(
          stage.get(), bound.value().paths.final_path);
      if (!published) {
        return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
            published.error());
      }
    }
    stage_cleanup.release();

    const auto published_path_matches = verify_opened_path(
        stage.get(),
        bound.value().paths.final_path,
        L"初回安全ガイドpublish後同一handle実体path");
    auto published_observation = observe_regular_single_link_file(
        stage.get(), L"初回安全ガイドpublish後同一handle属性");
    if (!published_path_matches || !published_observation ||
        !same_published_file(
            stage_complete.value(), published_observation.value())) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          !published_path_matches
              ? published_path_matches.error()
              : !published_observation
                    ? published_observation.error()
                    : guidance_error(
                          clonecore::ErrorCode::identity_mismatch,
                          ERROR_FILE_INVALID,
                          L"初回安全ガイドpublish後同一handle再識別",
                          L"完成設定のFile ID、寸法またはlink数が一致しません"));
    }
    auto published_bytes = read_bounded_file(
        stage.get(),
        published_observation.value().size_bytes,
        L"初回安全ガイドpublish後同一handle全byte読戻し");
    if (!published_bytes || published_bytes.value().size() !=
                                document.size() ||
        !std::equal(
            document.begin(), document.end(), published_bytes.value().begin())) {
      return published_bytes
          ? guidance_failure<FirstRunGuidanceSaveReport>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"初回安全ガイドpublish後同一handle全byte読戻し",
                L"完成設定のschema v1全byteが一致しません")
          : clonecore::Result<FirstRunGuidanceSaveReport>::failure(
                published_bytes.error());
    }
    if (FlushFileBuffers(stage.get()) == FALSE) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"初回安全ガイドpublish後同一handle flush",
              GetLastError()));
    }

    const auto directory_after_publish = reidentify_directory_chain(
        bound.value(), L"初回安全ガイドpublish後ancestor再識別");
    if (!directory_after_publish) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          directory_after_publish.error());
    }
    stage.reset();

    auto committed = load_bound_state(bound.value());
    if (committed.inspection.state !=
            FirstRunGuidanceDocumentState::acknowledged ||
        !committed.file.has_value() ||
        !same_published_file(
            stage_complete.value(), committed.file.value()) ||
        committed.bytes.size() != document.size() ||
        !std::equal(
            document.begin(), document.end(), committed.bytes.begin())) {
      return guidance_failure<FirstRunGuidanceSaveReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"初回安全ガイドpublish後完全再識別",
          L"完成設定のFile ID、寸法またはschema v1全byteが一致しません");
    }

    bool recovery_backup_retained = false;
    if (replacing) {
      const auto backup_path_matches = verify_opened_path(
          current.handle.get(),
          bound.value().paths.backup_path,
          L"初回安全ガイドrecovery backup破棄前実体path");
      auto backup_observed = observe_regular_single_link_file(
          current.handle.get(),
          L"初回安全ガイドrecovery backup破棄前属性");
      if (!backup_path_matches || !backup_observed ||
          !same_renamed_file(
              current.file.value(), backup_observed.value())) {
        recovery_backup_retained = true;
      } else {
        auto backup_bytes = read_bounded_file(
            current.handle.get(),
            backup_observed.value().size_bytes,
            L"初回安全ガイドrecovery backup同一handle読戻し");
        if (!backup_bytes || backup_bytes.value() != current.bytes ||
            !mark_delete_pending(
                current.handle.get(),
                L"初回安全ガイド検証済みrecovery backup破棄")) {
          recovery_backup_retained = true;
        }
      }
      current.handle.reset();
    }
    return clonecore::Result<FirstRunGuidanceSaveReport>::success({
        .disposition = replacing
            ? FirstRunGuidanceSaveDisposition::replaced
            : FirstRunGuidanceSaveDisposition::created,
        .final_path = bound.value().paths.final_path,
        .recovery_backup_retained = recovery_backup_retained,
    });
  } catch (const std::bad_alloc&) {
    return guidance_failure<FirstRunGuidanceSaveReport>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"初回安全ガイド設定保存",
        L"固定小型設定の保存に必要なメモリを確保できません");
  } catch (...) {
    return guidance_failure<FirstRunGuidanceSaveReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"初回安全ガイド設定保存",
        L"設定を安全に保存できません");
  }
}

}  // namespace detail

FirstRunGuidanceInspection
inspect_windows_first_run_guidance() noexcept {
  try {
    auto before = current_existing_data_observation();
    if (!before) {
      return unavailable_inspection(before.error());
    }
    auto inspection = detail::
        inspect_first_run_guidance_in_existing_data_directory(
            before.value().data_directory);
    auto after = current_existing_data_observation();
    if (!after ||
        !same_backing_observation(before.value(), after.value())) {
      return unavailable_inspection(
          after
              ? guidance_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_FILE_INVALID,
                    L"初回安全ガイドcurrent EXE/data再識別",
                    L"読取り中にcurrent EXE隣dataのpathまたはdiskが変化しました")
              : after.error());
    }
    return inspection;
  } catch (const std::bad_alloc&) {
    return unavailable_inspection(guidance_error(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"初回安全ガイドcurrent EXE設定読取り",
        L"必要なメモリを確保できません"));
  } catch (...) {
    return unavailable_inspection(guidance_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"初回安全ガイドcurrent EXE設定読取り",
        L"設定保存先を安全に確認できません"));
  }
}

clonecore::Result<FirstRunGuidanceSaveReport>
save_windows_first_run_guidance_acknowledgement() noexcept {
  try {
    auto before = current_existing_data_observation();
    if (!before) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          before.error());
    }
    auto saved = detail::
        save_first_run_guidance_in_existing_data_directory(
            before.value().data_directory);
    if (!saved) {
      return saved;
    }
    auto after = current_existing_data_observation();
    if (!after ||
        !same_backing_observation(before.value(), after.value())) {
      return clonecore::Result<FirstRunGuidanceSaveReport>::failure(
          after
              ? guidance_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_FILE_INVALID,
                    L"初回安全ガイドcurrent EXE/data保存後再識別",
                    L"保存中にcurrent EXE隣dataのpathまたはdiskが変化しました")
              : after.error());
    }
    return saved;
  } catch (const std::bad_alloc&) {
    return guidance_failure<FirstRunGuidanceSaveReport>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"初回安全ガイドcurrent EXE設定保存",
        L"必要なメモリを確保できません");
  } catch (...) {
    return guidance_failure<FirstRunGuidanceSaveReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"初回安全ガイドcurrent EXE設定保存",
        L"設定保存先を安全に確認できません");
  }
}

}  // namespace ytec::windowsapp

#include "ytec/windowsapp/startup_data_policy.h"

#include "ytec/clonecore/error.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kMaximumWindowsPathCharacters = 32U * 1024U;
constexpr unsigned int kMaximumProbeAttempts = 16U;

struct NormalizedLocalPath final {
  wchar_t drive{};
  std::vector<std::wstring> components;
  std::wstring canonical;
};

clonecore::Error portable_path_error(
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_argument,
      .native_code = native_code,
      .operation = L"イメージ保存先のPortable data境界確認",
      .message = std::move(message),
  };
}

bool ordinal_equal_ignore_case(
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

bool is_reserved_dos_component(const std::wstring_view component) noexcept {
  const std::size_t dot = component.find(L'.');
  const std::wstring_view stem = component.substr(0U, dot);
  if (ordinal_equal_ignore_case(stem, L"CON") ||
      ordinal_equal_ignore_case(stem, L"PRN") ||
      ordinal_equal_ignore_case(stem, L"AUX") ||
      ordinal_equal_ignore_case(stem, L"NUL")) {
    return true;
  }
  if (stem.size() == 4U && stem[3] >= L'1' && stem[3] <= L'9') {
    const std::wstring_view prefix = stem.substr(0U, 3U);
    return ordinal_equal_ignore_case(prefix, L"COM") ||
           ordinal_equal_ignore_case(prefix, L"LPT");
  }
  return false;
}

bool valid_windows_component(const std::wstring_view component) noexcept {
  if (component.empty() || component.back() == L' ' ||
      component.back() == L'.' || is_reserved_dos_component(component)) {
    return false;
  }
  for (const wchar_t character : component) {
    if (character < L' ' || character == L'<' || character == L'>' ||
        character == L':' || character == L'"' || character == L'|' ||
        character == L'?' || character == L'*') {
      return false;
    }
  }
  return true;
}

clonecore::Result<NormalizedLocalPath> normalize_local_windows_path(
    const std::wstring_view input,
    const std::wstring_view description) {
  const auto fail = [&](std::wstring detail) {
    return clonecore::Result<NormalizedLocalPath>::failure(
        portable_path_error(
            ERROR_INVALID_NAME,
            std::wstring(description) + L"を安全な絶対Windowsパスとして扱えません: " +
                std::move(detail)));
  };
  if (input.size() < 3U || input.size() >= kMaximumWindowsPathCharacters ||
      input.find(L'\0') != std::wstring_view::npos) {
    return fail(L"長さまたは文字列終端が不正です");
  }
  const wchar_t drive = input[0];
  if (!((drive >= L'A' && drive <= L'Z') ||
        (drive >= L'a' && drive <= L'z')) ||
      input[1] != L':' || (input[2] != L'\\' && input[2] != L'/')) {
    return fail(L"ローカルドライブのルートから始まっていません");
  }

  NormalizedLocalPath result{};
  result.drive = drive >= L'a' && drive <= L'z'
      ? static_cast<wchar_t>(drive - L'a' + L'A')
      : drive;
  std::size_t cursor = 3U;
  while (cursor < input.size()) {
    const std::size_t begin = cursor;
    while (cursor < input.size() && input[cursor] != L'\\' &&
           input[cursor] != L'/') {
      ++cursor;
    }
    const std::wstring_view component = input.substr(begin, cursor - begin);
    if (component.empty()) {
      return fail(L"空のパス要素があります");
    }
    if (component == L".") {
      // A current-directory element is unambiguous after lexical removal.
    } else if (component == L"..") {
      if (result.components.empty()) {
        return fail(L"ルートより上へ移動する要素があります");
      }
      result.components.pop_back();
    } else {
      if (!valid_windows_component(component)) {
        return fail(L"Windowsで曖昧または予約済みのパス要素があります");
      }
      result.components.emplace_back(component);
    }
    if (cursor < input.size()) {
      ++cursor;
      if (cursor == input.size()) {
        return fail(L"ファイルパスが区切り文字で終わっています");
      }
    }
  }

  result.canonical.reserve(input.size());
  result.canonical.push_back(result.drive);
  result.canonical += L":\\";
  for (std::size_t index = 0U; index < result.components.size(); ++index) {
    if (index != 0U) {
      result.canonical.push_back(L'\\');
    }
    result.canonical += result.components[index];
  }
  return clonecore::Result<NormalizedLocalPath>::success(std::move(result));
}

bool path_is_inside_or_equal(
    const NormalizedLocalPath& candidate,
    const NormalizedLocalPath& directory) noexcept {
  if (candidate.drive != directory.drive ||
      candidate.components.size() < directory.components.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < directory.components.size(); ++index) {
    if (!ordinal_equal_ignore_case(
            candidate.components[index], directory.components[index])) {
      return false;
    }
  }
  return true;
}

StartupDataPolicy blocked_policy(
    const StartupDataIssue issue,
    std::wstring data_directory,
    const clonecore::Error& error) {
  return StartupDataPolicy{
      .storage = StartupDataStorage::unavailable,
      .issue = issue,
      .data_directory = std::move(data_directory),
      .diagnostic = error.operation + L": " + error.message,
      .native_code = error.native_code,
  };
}

clonecore::Error invalid_path_error() {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_NAME,
      .operation = L"アプリ配置場所の確認",
      .message = L"EXEの親フォルダーを安全に識別できません",
  };
}

class UniqueHandle final {
 public:
  explicit UniqueHandle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
      : handle_(handle) {}

  ~UniqueHandle() {
    if (valid()) {
      static_cast<void>(CloseHandle(handle_));
    }
  }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  [[nodiscard]] bool close() noexcept {
    if (!valid()) {
      return true;
    }
    const HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return CloseHandle(handle) != FALSE;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

class WindowsStartupDataPlatform final : public IStartupDataPlatform {
 public:
  clonecore::Result<std::wstring> executable_path() override {
    std::vector<wchar_t> buffer(1024U, L'\0');
    for (;;) {
      SetLastError(ERROR_SUCCESS);
      const DWORD copied = GetModuleFileNameW(
          nullptr,
          buffer.data(),
          static_cast<DWORD>(buffer.size()));
      if (copied == 0U) {
        return clonecore::Result<std::wstring>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::query_failed,
                L"アプリ配置場所の確認",
                GetLastError()));
      }
      if (copied < buffer.size()) {
        return clonecore::Result<std::wstring>::success(
            std::wstring(buffer.data(), copied));
      }
      if (buffer.size() >= kMaximumWindowsPathCharacters) {
        return clonecore::Result<std::wstring>::failure({
            .code = clonecore::ErrorCode::query_failed,
            .native_code = ERROR_INSUFFICIENT_BUFFER,
            .operation = L"アプリ配置場所の確認",
            .message = L"EXEの完全パスを安全に取得できません",
        });
      }
      buffer.resize(
          (std::min)(
              buffer.size() * 2U,
              kMaximumWindowsPathCharacters),
          L'\0');
    }
  }

  clonecore::Status require_regular_non_reparse_directory(
      const std::wstring& path) override {
    UniqueHandle directory(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!directory.valid()) {
      return clonecore::Status::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"dataフォルダーの安全確認",
              GetLastError()));
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(
            directory.get(),
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) == FALSE) {
      return clonecore::Status::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"dataフォルダーの属性確認",
              GetLastError()));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Status::failure({
          .code = clonecore::ErrorCode::invalid_data,
          .native_code = ERROR_REPARSE_TAG_INVALID,
          .operation = L"dataフォルダーの安全確認",
          .message =
              L"通常の非reparseディレクトリとして確認できません",
      });
    }
    return clonecore::success_status();
  }

  clonecore::Status create_directory_if_missing(
      const std::wstring& path) override {
    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
      return clonecore::success_status();
    }
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
      return clonecore::success_status();
    }
    return clonecore::Status::failure(
        clonecore::make_win32_error(
            error == ERROR_ACCESS_DENIED
                ? clonecore::ErrorCode::access_denied
                : clonecore::ErrorCode::io_failed,
            L"EXE隣dataフォルダーの作成",
            error));
  }

  clonecore::Status verify_directory_write_access(
      const std::wstring& path) override {
    constexpr std::array<std::byte, 16U> marker{
        std::byte{0x59}, std::byte{0x54}, std::byte{0x45}, std::byte{0x43},
        std::byte{0x2D}, std::byte{0x44}, std::byte{0x41}, std::byte{0x54},
        std::byte{0x41}, std::byte{0x2D}, std::byte{0x50}, std::byte{0x52},
        std::byte{0x4F}, std::byte{0x42}, std::byte{0x45}, std::byte{0x31},
    };

    for (unsigned int attempt = 0U;
         attempt < kMaximumProbeAttempts;
         ++attempt) {
      const std::wstring probe_path =
          path + L"\\.tsumugi-write-probe-" +
          std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(GetCurrentThreadId()) + L"-" +
          std::to_wstring(GetTickCount64()) + L"-" +
          std::to_wstring(attempt) + L".tmp";
      UniqueHandle probe(CreateFileW(
          probe_path.c_str(),
          GENERIC_READ | GENERIC_WRITE | DELETE,
          0,
          nullptr,
          CREATE_NEW,
          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
              FILE_FLAG_WRITE_THROUGH,
          nullptr));
      if (!probe.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
          continue;
        }
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                error == ERROR_ACCESS_DENIED
                    ? clonecore::ErrorCode::access_denied
                    : clonecore::ErrorCode::io_failed,
                L"EXE隣dataフォルダーの書込み確認",
                error));
      }

      DWORD transferred = 0U;
      const BOOL write_succeeded = WriteFile(
          probe.get(),
          marker.data(),
          static_cast<DWORD>(marker.size()),
          &transferred,
          nullptr);
      const DWORD write_error =
          write_succeeded == FALSE ? GetLastError() : ERROR_SUCCESS;
      if (write_succeeded == FALSE ||
          transferred != static_cast<DWORD>(marker.size())) {
        const DWORD error = write_succeeded == FALSE
                                ? write_error
                                : ERROR_WRITE_FAULT;
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"EXE隣dataフォルダーの書込み確認",
                error));
      }
      if (FlushFileBuffers(probe.get()) == FALSE) {
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"EXE隣dataフォルダーのflush確認",
                GetLastError()));
      }
      LARGE_INTEGER start{};
      if (SetFilePointerEx(probe.get(), start, nullptr, FILE_BEGIN) == FALSE) {
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"EXE隣dataフォルダーの読戻し準備",
                GetLastError()));
      }
      std::array<std::byte, marker.size()> readback{};
      transferred = 0U;
      const BOOL read_succeeded = ReadFile(
          probe.get(),
          readback.data(),
          static_cast<DWORD>(readback.size()),
          &transferred,
          nullptr);
      const DWORD read_error =
          read_succeeded == FALSE ? GetLastError() : ERROR_SUCCESS;
      if (read_succeeded == FALSE ||
          transferred != static_cast<DWORD>(readback.size()) ||
          readback != marker) {
        const DWORD error = read_succeeded == FALSE
                                ? read_error
                                : ERROR_CRC;
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::verification_failed,
                L"EXE隣dataフォルダーの読戻し確認",
                error));
      }
      if (!probe.close()) {
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"EXE隣dataフォルダーの確認ファイル削除",
                GetLastError()));
      }
      SetLastError(ERROR_SUCCESS);
      const DWORD probe_attributes =
          GetFileAttributesW(probe_path.c_str());
      const DWORD attribute_error =
          probe_attributes == INVALID_FILE_ATTRIBUTES
              ? GetLastError()
              : ERROR_SUCCESS;
      if (probe_attributes != INVALID_FILE_ATTRIBUTES ||
          (attribute_error != ERROR_FILE_NOT_FOUND &&
           attribute_error != ERROR_PATH_NOT_FOUND)) {
        const DWORD error = probe_attributes != INVALID_FILE_ATTRIBUTES
                                ? ERROR_DELETE_PENDING
                                : attribute_error;
        return clonecore::Status::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::verification_failed,
                L"EXE隣dataフォルダーの確認ファイル削除",
                error));
      }
      return clonecore::success_status();
    }

    return clonecore::Status::failure({
        .code = clonecore::ErrorCode::io_failed,
        .native_code = ERROR_FILE_EXISTS,
        .operation = L"EXE隣dataフォルダーの書込み確認",
        .message = L"重複しない確認ファイル名を確保できません",
    });
  }
};

}  // namespace

clonecore::Result<TsumugiPortableDataPathProof>
evaluate_tsumugi_portable_data_path_gate(
    const std::wstring_view executable_path,
    const std::wstring_view final_path) noexcept {
  try {
    auto executable = normalize_local_windows_path(
        executable_path, L"実行中EXEのパス");
    if (!executable || executable.value().components.empty()) {
      return clonecore::Result<TsumugiPortableDataPathProof>::failure(
          executable
              ? portable_path_error(
                    ERROR_INVALID_NAME,
                    L"実行中EXEのファイル名を識別できません")
              : executable.error());
    }
    auto destination = normalize_local_windows_path(
        final_path, L".tsumugi完成ファイルのパス");
    if (!destination || destination.value().components.empty()) {
      return clonecore::Result<TsumugiPortableDataPathProof>::failure(
          destination
              ? portable_path_error(
                    ERROR_INVALID_NAME,
                    L".tsumugi完成ファイル名を識別できません")
              : destination.error());
    }

    constexpr std::wstring_view kTsumugiExtension{L".tsumugi"};
    const std::wstring& leaf = destination.value().components.back();
    if (leaf.size() <= kTsumugiExtension.size() ||
        !ordinal_equal_ignore_case(
            std::wstring_view(leaf).substr(
                leaf.size() - kTsumugiExtension.size()),
            kTsumugiExtension)) {
      return clonecore::Result<TsumugiPortableDataPathProof>::failure(
          portable_path_error(
              ERROR_INVALID_NAME,
              L"完成ファイルは.tsumugi拡張子の新しい絶対パスで指定してください"));
    }

    NormalizedLocalPath data_directory{};
    data_directory.drive = executable.value().drive;
    data_directory.components = executable.value().components;
    data_directory.components.pop_back();
    data_directory.components.emplace_back(L"data");
    data_directory.canonical.push_back(data_directory.drive);
    data_directory.canonical += L":\\";
    for (std::size_t index = 0U;
         index < data_directory.components.size();
         ++index) {
      if (index != 0U) {
        data_directory.canonical.push_back(L'\\');
      }
      data_directory.canonical += data_directory.components[index];
    }

    if (destination.value().canonical.size() >
        kMaximumWindowsPathCharacters -
            std::wstring_view(L".partial").size() - 1U) {
      return clonecore::Result<TsumugiPortableDataPathProof>::failure(
          portable_path_error(
              ERROR_FILENAME_EXCED_RANGE,
              L"隣接.partial名がWindowsパス上限を超えます"));
    }
    auto partial = normalize_local_windows_path(
        destination.value().canonical + L".partial",
        L"隣接.partialファイルのパス");
    if (!partial) {
      return clonecore::Result<TsumugiPortableDataPathProof>::failure(
          partial.error());
    }

    if (path_is_inside_or_equal(destination.value(), data_directory) ||
        path_is_inside_or_equal(partial.value(), data_directory)) {
      return clonecore::Result<TsumugiPortableDataPathProof>::failure(
          portable_path_error(
              ERROR_ACCESS_DENIED,
              L".tsumugiと隣接.partialはEXE隣dataまたはその配下へ保存できません。別のローカルフォルダーを選択してください"));
    }

    return clonecore::Result<TsumugiPortableDataPathProof>::success({
        .canonical_data_directory =
            std::move(data_directory.canonical),
        .canonical_final_path =
            std::move(destination.value().canonical),
        .canonical_partial_path =
            std::move(partial.value().canonical),
    });
  } catch (const std::bad_alloc&) {
    return clonecore::Result<TsumugiPortableDataPathProof>::failure(
        portable_path_error(
            ERROR_NOT_ENOUGH_MEMORY,
            L"保存先境界の確認に必要なメモリを確保できません"));
  } catch (...) {
    return clonecore::Result<TsumugiPortableDataPathProof>::failure(
        portable_path_error(
            ERROR_UNHANDLED_EXCEPTION,
            L"保存先境界を安全に確認できません"));
  }
}

clonecore::Status
require_windows_tsumugi_destination_outside_portable_data(
    const std::wstring& final_path) noexcept {
  WindowsStartupDataPlatform platform;
  auto executable = platform.executable_path();
  if (!executable) {
    return clonecore::Status::failure(executable.error());
  }
  auto proof = evaluate_tsumugi_portable_data_path_gate(
      executable.value(), final_path);
  if (!proof) {
    return clonecore::Status::failure(proof.error());
  }
  return clonecore::success_status();
}

StartupDataPolicy evaluate_startup_data_policy(
    IStartupDataPlatform& platform) noexcept {
  try {
    auto executable = platform.executable_path();
    if (!executable) {
      return blocked_policy(
          StartupDataIssue::executable_path_unavailable,
          {},
          executable.error());
    }

    const std::filesystem::path executable_file(executable.value());
    if (!executable_file.is_absolute() || !executable_file.has_filename() ||
        !executable_file.has_parent_path()) {
      return blocked_policy(
          StartupDataIssue::executable_path_invalid,
          {},
          invalid_path_error());
    }
    const std::filesystem::path application_directory =
        executable_file.parent_path();
    const std::filesystem::path data_directory =
        application_directory / L"data";
    const std::wstring application_path = application_directory.native();
    const std::wstring data_path = data_directory.native();

    const auto application_safe =
        platform.require_regular_non_reparse_directory(application_path);
    if (!application_safe) {
      return blocked_policy(
          StartupDataIssue::application_directory_unsafe,
          data_path,
          application_safe.error());
    }
    const auto created = platform.create_directory_if_missing(data_path);
    if (!created) {
      return blocked_policy(
          StartupDataIssue::data_directory_unavailable,
          data_path,
          created.error());
    }
    const auto data_safe =
        platform.require_regular_non_reparse_directory(data_path);
    if (!data_safe) {
      return blocked_policy(
          StartupDataIssue::data_directory_unsafe,
          data_path,
          data_safe.error());
    }
    const auto writable = platform.verify_directory_write_access(data_path);
    if (!writable) {
      return blocked_policy(
          StartupDataIssue::write_probe_failed,
          data_path,
          writable.error());
    }
    // Re-check after the write probe so a changed/replaced directory never
    // leaves the application in write-capable mode.
    const auto data_still_safe =
        platform.require_regular_non_reparse_directory(data_path);
    if (!data_still_safe) {
      return blocked_policy(
          StartupDataIssue::data_directory_unsafe,
          data_path,
          data_still_safe.error());
    }
    return StartupDataPolicy{
        .storage = StartupDataStorage::persistent_data,
        .issue = StartupDataIssue::none,
        .data_directory = data_path,
        .diagnostic = {},
        .native_code = ERROR_SUCCESS,
    };
  } catch (const std::bad_alloc&) {
    return StartupDataPolicy{
        .storage = StartupDataStorage::unavailable,
        .issue = StartupDataIssue::unexpected_failure,
        .diagnostic = L"起動時のdataフォルダー確認でメモリを確保できません",
        .native_code = ERROR_NOT_ENOUGH_MEMORY,
    };
  } catch (...) {
    return StartupDataPolicy{
        .storage = StartupDataStorage::unavailable,
        .issue = StartupDataIssue::unexpected_failure,
        .diagnostic = L"起動時のdataフォルダー確認を完了できません",
        .native_code = ERROR_UNHANDLED_EXCEPTION,
    };
  }
}

StartupDataPolicy make_read_only_bootstrap_data_policy() noexcept {
  return StartupDataPolicy{
      .storage = StartupDataStorage::bounded_ram,
      .issue = StartupDataIssue::none,
      .data_directory = {},
      .diagnostic =
          L"操作対象を識別するまで診断ログをbounded RAM内だけに保持します",
      .native_code = ERROR_SUCCESS,
  };
}

StartupDataBackingRelationship classify_startup_data_backing(
    const std::optional<std::uint32_t> backing_disk_number,
    const std::span<const std::uint32_t> protected_disk_numbers) noexcept {
  if (!backing_disk_number.has_value() || protected_disk_numbers.empty()) {
    return StartupDataBackingRelationship::unknown;
  }
  return std::find(
             protected_disk_numbers.begin(),
             protected_disk_numbers.end(),
             backing_disk_number.value()) != protected_disk_numbers.end()
             ? StartupDataBackingRelationship::protected_disk
             : StartupDataBackingRelationship::disjoint;
}

clonecore::Result<StartupDataBackingObservation>
inspect_windows_startup_data_backing() noexcept {
  try {
    WindowsStartupDataPlatform platform;
    auto executable = platform.executable_path();
    if (!executable) {
      return clonecore::Result<StartupDataBackingObservation>::failure(
          executable.error());
    }
    const std::filesystem::path executable_file(executable.value());
    if (!executable_file.is_absolute() || !executable_file.has_filename() ||
        !executable_file.has_parent_path()) {
      return clonecore::Result<StartupDataBackingObservation>::failure(
          invalid_path_error());
    }

    const std::filesystem::path application_directory =
        executable_file.parent_path();
    const std::filesystem::path data_directory =
        application_directory / L"data";
    const std::wstring application_path = application_directory.native();
    const std::wstring data_path = data_directory.native();
    const auto application_safe =
        platform.require_regular_non_reparse_directory(application_path);
    if (!application_safe) {
      return clonecore::Result<StartupDataBackingObservation>::failure(
          application_safe.error());
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD data_attributes = GetFileAttributesW(data_path.c_str());
    const DWORD attribute_error =
        data_attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError()
            : ERROR_SUCCESS;
    bool data_exists = data_attributes != INVALID_FILE_ATTRIBUTES;
    if (!data_exists && attribute_error != ERROR_FILE_NOT_FOUND &&
        attribute_error != ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<StartupDataBackingObservation>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"EXE隣dataフォルダーの読取り専用確認",
              attribute_error));
    }

    std::wstring mapping_candidate = data_path;
    if (data_exists) {
      const auto data_safe =
          platform.require_regular_non_reparse_directory(data_path);
      if (!data_safe) {
        return clonecore::Result<StartupDataBackingObservation>::failure(
            data_safe.error());
      }
      // query_single_disk_number_for_local_path validates the candidate's
      // parent, so use a non-created child name to include data itself.
      mapping_candidate += L"\\.tsumugi-read-only-backing-observation";
    }
    auto disk_number =
        diskmodel::query_single_disk_number_for_local_path(mapping_candidate);
    if (!disk_number) {
      return clonecore::Result<StartupDataBackingObservation>::failure(
          disk_number.error());
    }
    return clonecore::Result<StartupDataBackingObservation>::success({
        .application_directory = application_path,
        .data_directory = data_path,
        .disk_number = disk_number.value(),
        .data_directory_exists = data_exists,
    });
  } catch (const std::bad_alloc&) {
    return clonecore::Result<StartupDataBackingObservation>::failure({
        .code = clonecore::ErrorCode::io_failed,
        .native_code = ERROR_NOT_ENOUGH_MEMORY,
        .operation = L"data保存先の読取り専用識別",
        .message = L"保存先の識別に必要なメモリを確保できません",
    });
  } catch (...) {
    return clonecore::Result<StartupDataBackingObservation>::failure({
        .code = clonecore::ErrorCode::query_failed,
        .native_code = ERROR_UNHANDLED_EXCEPTION,
        .operation = L"data保存先の読取り専用識別",
        .message = L"保存先を安全に識別できません",
    });
  }
}

StartupDataPolicy inspect_windows_startup_data_policy() noexcept {
  WindowsStartupDataPlatform platform;
  return evaluate_startup_data_policy(platform);
}

}  // namespace ytec::windowsapp

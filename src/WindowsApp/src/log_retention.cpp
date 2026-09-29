#include "ytec/windowsapp/log_retention.h"

#include "ytec/windowsapp/startup_data_policy.h"

#include "ytec/clonecore/error.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::uint64_t kFileTimeTicksPerDay =
    24U * 60U * 60U * 10'000'000U;
constexpr std::array<std::byte, 3U> kUtf8Bom{
    std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};

clonecore::Error retention_error(
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

bool take_digits(
    const std::wstring& value,
    std::size_t& cursor,
    const std::size_t count,
    std::uint32_t& parsed) noexcept {
  if (count == 0U || cursor > value.size() ||
      count > value.size() - cursor) {
    return false;
  }
  std::uint32_t result = 0U;
  for (std::size_t index = 0U; index < count; ++index) {
    const wchar_t character = value[cursor + index];
    if (character < L'0' || character > L'9') {
      return false;
    }
    result = result * 10U +
             static_cast<std::uint32_t>(character - L'0');
  }
  cursor += count;
  parsed = result;
  return true;
}

bool take_separator(
    const std::wstring& value,
    std::size_t& cursor,
    const wchar_t expected) noexcept {
  if (cursor >= value.size() || value[cursor] != expected) {
    return false;
  }
  ++cursor;
  return true;
}

bool valid_calendar_fields(
    const std::uint32_t year,
    const std::uint32_t month,
    const std::uint32_t day,
    const std::uint32_t hour,
    const std::uint32_t minute,
    const std::uint32_t second,
    const std::uint32_t milliseconds) noexcept {
  if (year < 2000U || year > 9999U || month < 1U || month > 12U ||
      hour > 23U || minute > 59U || second > 59U ||
      milliseconds > 999U) {
    return false;
  }
  constexpr std::array<std::uint32_t, 12U> kDaysPerMonth{
      31U, 28U, 31U, 30U, 31U, 30U,
      31U, 31U, 30U, 31U, 30U, 31U};
  std::uint32_t maximum_day = kDaysPerMonth[month - 1U];
  const bool leap_year =
      year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
  if (month == 2U && leap_year) {
    maximum_day = 29U;
  }
  return day >= 1U && day <= maximum_day;
}

bool retention_elapsed(
    const std::uint64_t last_write,
    const std::uint64_t now,
    const std::uint64_t days) noexcept {
  if (last_write > now ||
      days > (std::numeric_limits<std::uint64_t>::max)() /
                 kFileTimeTicksPerDay) {
    return false;
  }
  const std::uint64_t threshold = days * kFileTimeTicksPerDay;
  return now - last_write >= threshold;
}

std::uint64_t saturating_add(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return (std::numeric_limits<std::uint64_t>::max)();
  }
  return left + right;
}

bool observation_less(
    const ProductLogObservation& left,
    const ProductLogObservation& right) noexcept {
  if (left.last_write_utc_100ns != right.last_write_utc_100ns) {
    return left.last_write_utc_100ns < right.last_write_utc_100ns;
  }
  if (left.file_name != right.file_name) {
    return left.file_name < right.file_name;
  }
  if (left.volume_serial != right.volume_serial) {
    return left.volume_serial < right.volume_serial;
  }
  return left.file_id < right.file_id;
}

bool deletion_less(
    const ProductLogDeletion& left,
    const ProductLogDeletion& right) noexcept {
  return observation_less(left.observed, right.observed);
}

struct HandleObservation final {
  std::uint64_t size_bytes{};
  std::uint64_t last_write_utc_100ns{};
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};
  bool regular_file{};
  bool reparse_point{};
  bool has_utf8_bom{};
};

class UniqueFindHandle final {
 public:
  explicit UniqueFindHandle(
      const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
      : handle_(handle) {}

  ~UniqueFindHandle() {
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

clonecore::Result<HandleObservation> observe_log_handle(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  FILE_ID_INFO identity{};
  if (GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE ||
      GetFileInformationByHandleEx(
          handle,
          FileStandardInfo,
          &standard,
          sizeof(standard)) == FALSE ||
      GetFileInformationByHandleEx(
          handle,
          FileBasicInfo,
          &basic,
          sizeof(basic)) == FALSE ||
      GetFileInformationByHandleEx(
          handle,
          FileIdInfo,
          &identity,
          sizeof(identity)) == FALSE) {
    return clonecore::Result<HandleObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if (standard.EndOfFile.QuadPart < 0 ||
      basic.LastWriteTime.QuadPart < 0) {
    return clonecore::Result<HandleObservation>::failure(retention_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"ログのファイル長または更新時刻が不正です"));
  }

  HandleObservation result{};
  result.size_bytes =
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  result.last_write_utc_100ns =
      static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
  result.volume_serial = identity.VolumeSerialNumber;
  std::memcpy(
      result.file_id.data(),
      identity.FileId.Identifier,
      result.file_id.size());
  result.reparse_point =
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
  result.regular_file =
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
      (attributes.FileAttributes & FILE_ATTRIBUTE_DEVICE) == 0U &&
      !result.reparse_point && standard.NumberOfLinks == 1U &&
      standard.DeletePending == FALSE;

  if (result.regular_file && result.size_bytes >= kUtf8Bom.size()) {
    std::array<std::byte, kUtf8Bom.size()> prefix{};
    DWORD read = 0U;
    if (ReadFile(
            handle,
            prefix.data(),
            static_cast<DWORD>(prefix.size()),
            &read,
            nullptr) == FALSE) {
      return clonecore::Result<HandleObservation>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              operation,
              GetLastError()));
    }
    result.has_utf8_bom =
        read == static_cast<DWORD>(prefix.size()) && prefix == kUtf8Bom;
  }
  return clonecore::Result<HandleObservation>::success(result);
}

bool unchanged(
    const HandleObservation& current,
    const ProductLogObservation& expected) noexcept {
  return current.regular_file && !current.reparse_point &&
         current.has_utf8_bom && expected.regular_file &&
         !expected.reparse_point && expected.has_utf8_bom &&
         current.size_bytes == expected.size_bytes &&
         current.last_write_utc_100ns ==
             expected.last_write_utc_100ns &&
         current.volume_serial == expected.volume_serial &&
         current.file_id == expected.file_id;
}

clonecore::Status require_regular_non_reparse_directory(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle directory(CreateFileW(
      path.c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        operation,
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (GetFileInformationByHandleEx(
          directory.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        operation,
        GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Status::failure(retention_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparseディレクトリとして確認できません"));
  }
  return clonecore::success_status();
}

class WindowsProductLogRetentionPlatform final
    : public IProductLogRetentionPlatform {
 public:
  clonecore::Result<std::vector<ProductLogObservation>>
  enumerate_owned_candidates(
      const std::wstring& log_directory) override {
    const auto directory_safe = require_regular_non_reparse_directory(
        log_directory, L"製品ログフォルダーの列挙前確認");
    if (!directory_safe) {
      return clonecore::Result<
          std::vector<ProductLogObservation>>::failure(
          directory_safe.error());
    }

    const std::wstring pattern = log_directory + L"\\*";
    WIN32_FIND_DATAW found{};
    UniqueFindHandle find(FindFirstFileW(pattern.c_str(), &found));
    if (!find.valid()) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND) {
        return clonecore::Result<
            std::vector<ProductLogObservation>>::success({});
      }
      return clonecore::Result<
          std::vector<ProductLogObservation>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"製品ログフォルダーの列挙",
              error));
    }

    std::vector<ProductLogObservation> result;
    for (;;) {
      const std::wstring file_name(found.cFileName);
      if (classify_product_log_file_name(file_name).has_value()) {
        const std::wstring path = log_directory + L"\\" + file_name;
        clonecore::UniqueHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!file) {
          const DWORD error = GetLastError();
          return clonecore::Result<
              std::vector<ProductLogObservation>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::query_failed,
                  L"製品ログ候補の読取り専用確認",
                  error));
        }
        auto observed = observe_log_handle(
            file.get(), L"製品ログ候補の読取り専用確認");
        if (!observed) {
          return clonecore::Result<
              std::vector<ProductLogObservation>>::failure(
              observed.error());
        }
        const auto& value = observed.value();
        result.push_back(ProductLogObservation{
            .file_name = file_name,
            .size_bytes = value.size_bytes,
            .last_write_utc_100ns = value.last_write_utc_100ns,
            .volume_serial = value.volume_serial,
            .file_id = value.file_id,
            .regular_file = value.regular_file,
            .reparse_point = value.reparse_point,
            .has_utf8_bom = value.has_utf8_bom,
        });
      }

      if (FindNextFileW(find.get(), &found) == FALSE) {
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
          return clonecore::Result<
              std::vector<ProductLogObservation>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::enumeration_failed,
                  L"製品ログフォルダーの列挙",
                  error));
        }
        break;
      }
    }
    return clonecore::Result<
        std::vector<ProductLogObservation>>::success(std::move(result));
  }

  clonecore::Status delete_if_unchanged(
      const std::wstring& log_directory,
      const ProductLogObservation& expected) override {
    if (!classify_product_log_file_name(expected.file_name).has_value()) {
      return clonecore::Status::failure(retention_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          L"製品ログ削除前確認",
          L"製品所有と分類できない名前です"));
    }
    const auto directory_safe = require_regular_non_reparse_directory(
        log_directory, L"製品ログ削除前のフォルダー再確認");
    if (!directory_safe) {
      return directory_safe;
    }

    const std::wstring path =
        log_directory + L"\\" + expected.file_name;
    clonecore::UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!file) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"製品ログ削除前の再オープン",
          GetLastError()));
    }
    const auto current = observe_log_handle(
        file.get(), L"製品ログ削除直前の再識別");
    if (!current) {
      return clonecore::Status::failure(current.error());
    }
    if (!unchanged(current.value(), expected)) {
      return clonecore::Status::failure(retention_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"製品ログ削除直前の再識別",
          L"列挙後にログの識別情報、長さ、更新時刻、または属性が変化しました"));
    }

    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(
            file.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition)) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"期限切れ製品ログの削除",
          GetLastError()));
    }
    return clonecore::success_status();
  }
};

clonecore::Result<clonecore::UniqueHandle> open_pinned_log_directory(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle directory(CreateFileW(
      path.c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (GetFileInformationByHandleEx(
          directory.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        retention_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_REPARSE_TAG_INVALID,
            std::wstring(operation),
            L"通常の非reparseディレクトリとして固定できません"));
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      std::move(directory));
}

class WindowsProductLogCompletionPlatform final
    : public IProductLogCompletionPlatform {
 public:
  clonecore::Status promote_failure_no_replace(
      const std::wstring& log_directory,
      const std::wstring& failed_file_name,
      const std::wstring& normal_file_name) override {
    if (classify_product_log_file_name(failed_file_name) !=
            ProductLogClass::failure ||
        classify_product_log_file_name(normal_file_name) !=
            ProductLogClass::normal ||
        failed_file_name.find_first_of(L"\\/") != std::wstring::npos ||
        normal_file_name.find_first_of(L"\\/") != std::wstring::npos) {
      return clonecore::Status::failure(retention_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          L"製品ログ完了分類",
          L"製品所有のfailed／normalログ名として確認できません"));
    }

    auto pinned_directory = open_pinned_log_directory(
        log_directory, L"製品ログ完了分類のlogs固定");
    if (!pinned_directory) {
      return clonecore::Status::failure(pinned_directory.error());
    }

    const std::wstring failed_path =
        log_directory + L"\\" + failed_file_name;
    const std::wstring normal_path =
        log_directory + L"\\" + normal_file_name;
    clonecore::UniqueHandle failed_file(CreateFileW(
        failed_path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!failed_file) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"failed製品ログのハンドル固定",
          GetLastError()));
    }
    const auto observed = observe_log_handle(
        failed_file.get(), L"failed製品ログの完了直前再識別");
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    if (!observed.value().regular_file ||
        observed.value().reparse_point ||
        !observed.value().has_utf8_bom) {
      return clonecore::Status::failure(retention_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"failed製品ログの完了直前再識別",
          L"単一linkの通常UTF-8製品ログとして確認できません"));
    }

    SetLastError(ERROR_SUCCESS);
    clonecore::UniqueHandle existing(CreateFileW(
        normal_path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (existing) {
      return clonecore::Status::failure(retention_error(
          clonecore::ErrorCode::confirmation_required,
          ERROR_FILE_EXISTS,
          L"normal製品ログの非上書き確認",
          L"同じnormalログ名が既に存在するためfailedログを保持します"));
    }
    const DWORD existence_error = GetLastError();
    if (existence_error != ERROR_FILE_NOT_FOUND) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"normal製品ログの非上書き確認",
          existence_error));
    }

    const std::size_t name_bytes = normal_path.size() * sizeof(wchar_t);
    const std::size_t buffer_bytes =
        offsetof(FILE_RENAME_INFO, FileName) + name_bytes +
        sizeof(wchar_t);
    if (name_bytes > (std::numeric_limits<DWORD>::max)() ||
        buffer_bytes > (std::numeric_limits<DWORD>::max)()) {
      return clonecore::Status::failure(retention_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_FILENAME_EXCED_RANGE,
          L"normal製品ログ名の構成",
          L"完成ログ名がWindows API上限を超えています"));
    }
    std::vector<std::byte> buffer(buffer_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, normal_path.data(), name_bytes);
    if (SetFileInformationByHandle(
            failed_file.get(),
            FileRenameInfo,
            buffer.data(),
            static_cast<DWORD>(buffer.size())) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"製品ログのハンドル非上書き完了分類",
          GetLastError()));
    }
    return clonecore::success_status();
  }
};

}  // namespace

std::optional<ProductLogClass> classify_product_log_file_name(
    const std::wstring& file_name) noexcept {
  constexpr std::wstring_view kPrefix{L"TsumugiDrive-"};
  constexpr std::wstring_view kNormalTag{L"normal-"};
  constexpr std::wstring_view kFailureTag{L"failed-"};
  constexpr std::wstring_view kExtension{L".log"};
  if (!file_name.starts_with(kPrefix)) {
    return std::nullopt;
  }
  std::size_t cursor = kPrefix.size();
  ProductLogClass log_class = ProductLogClass::normal;
  const std::wstring_view remaining(file_name.c_str() + cursor);
  if (remaining.starts_with(kNormalTag)) {
    cursor += kNormalTag.size();
  } else if (remaining.starts_with(kFailureTag)) {
    cursor += kFailureTag.size();
    log_class = ProductLogClass::failure;
  }

  std::uint32_t year{};
  std::uint32_t month{};
  std::uint32_t day{};
  std::uint32_t hour{};
  std::uint32_t minute{};
  std::uint32_t second{};
  std::uint32_t milliseconds{};
  if (!take_digits(file_name, cursor, 4U, year) ||
      !take_digits(file_name, cursor, 2U, month) ||
      !take_digits(file_name, cursor, 2U, day) ||
      !take_separator(file_name, cursor, L'-') ||
      !take_digits(file_name, cursor, 2U, hour) ||
      !take_digits(file_name, cursor, 2U, minute) ||
      !take_digits(file_name, cursor, 2U, second) ||
      !take_separator(file_name, cursor, L'-') ||
      !take_digits(file_name, cursor, 3U, milliseconds) ||
      !take_separator(file_name, cursor, L'-') ||
      !valid_calendar_fields(
          year,
          month,
          day,
          hour,
          minute,
          second,
          milliseconds)) {
    return std::nullopt;
  }

  const std::size_t process_begin = cursor;
  std::uint64_t process_id = 0U;
  while (cursor < file_name.size() &&
         file_name[cursor] >= L'0' && file_name[cursor] <= L'9') {
    process_id = process_id * 10U +
                 static_cast<std::uint64_t>(file_name[cursor] - L'0');
    ++cursor;
  }
  const std::size_t process_length = cursor - process_begin;
  if (process_length == 0U || process_length > 10U || process_id == 0U ||
      process_id > (std::numeric_limits<DWORD>::max)()) {
    return std::nullopt;
  }
  if (cursor < file_name.size() && file_name[cursor] == L'-') {
    ++cursor;
    if (cursor >= file_name.size() || file_name[cursor] < L'1' ||
        file_name[cursor] > L'9') {
      return std::nullopt;
    }
    ++cursor;
  }
  if (cursor > file_name.size() ||
      file_name.size() - cursor != kExtension.size() ||
      std::wstring_view(file_name).substr(cursor) != kExtension) {
    return std::nullopt;
  }
  return log_class;
}

clonecore::Result<ProductLogCompletionPlan> plan_product_log_completion(
    const std::wstring& failed_file_name,
    const bool clean_close_requested,
    const bool error_detected,
    const bool active_write_operation) noexcept {
  try {
    constexpr std::wstring_view kFailurePrefix{L"TsumugiDrive-failed-"};
    constexpr std::wstring_view kNormalPrefix{L"TsumugiDrive-normal-"};
    if (classify_product_log_file_name(failed_file_name) !=
            ProductLogClass::failure ||
        !std::wstring_view(failed_file_name).starts_with(kFailurePrefix)) {
      return clonecore::Result<ProductLogCompletionPlan>::failure(
          retention_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_NAME,
              L"製品ログ完了分類計画",
              L"failed-first製品ログ名として確認できません"));
    }

    ProductLogCompletionPlan plan{
        .disposition = ProductLogCompletionDisposition::keep_failure,
        .failed_file_name = failed_file_name,
    };
    if (!clean_close_requested || error_detected || active_write_operation) {
      return clonecore::Result<ProductLogCompletionPlan>::success(
          std::move(plan));
    }

    plan.normal_file_name = std::wstring(kNormalPrefix) +
        failed_file_name.substr(kFailurePrefix.size());
    if (classify_product_log_file_name(plan.normal_file_name) !=
        ProductLogClass::normal) {
      return clonecore::Result<ProductLogCompletionPlan>::failure(
          retention_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_NAME,
              L"製品ログ完了分類計画",
              L"対応するnormal製品ログ名を安全に構成できません"));
    }
    plan.disposition =
        ProductLogCompletionDisposition::promote_to_normal;
    return clonecore::Result<ProductLogCompletionPlan>::success(
        std::move(plan));
  } catch (const std::bad_alloc&) {
    return clonecore::Result<ProductLogCompletionPlan>::failure(
        retention_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"製品ログ完了分類計画",
            L"完了分類に必要なメモリを確保できません"));
  } catch (...) {
    return clonecore::Result<ProductLogCompletionPlan>::failure(
        retention_error(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"製品ログ完了分類計画",
            L"完了分類を安全に構成できません"));
  }
}

clonecore::Result<ProductLogCompletionReport>
complete_product_log_session(
    IProductLogCompletionPlatform& platform,
    const std::wstring& log_directory,
    const std::wstring& failed_file_name,
    const bool clean_close_requested,
    const bool error_detected,
    const bool active_write_operation) {
  auto plan = plan_product_log_completion(
      failed_file_name,
      clean_close_requested,
      error_detected,
      active_write_operation);
  if (!plan) {
    return clonecore::Result<ProductLogCompletionReport>::failure(
        plan.error());
  }
  ProductLogCompletionReport report{
      .plan = plan.take_value(),
  };
  if (report.plan.disposition ==
      ProductLogCompletionDisposition::keep_failure) {
    return clonecore::Result<ProductLogCompletionReport>::success(
        std::move(report));
  }
  if (log_directory.empty()) {
    return clonecore::Result<ProductLogCompletionReport>::failure(
        retention_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"製品ログ完了分類",
            L"logsフォルダーが空です"));
  }
  const auto promoted = platform.promote_failure_no_replace(
      log_directory,
      report.plan.failed_file_name,
      report.plan.normal_file_name);
  if (!promoted) {
    return clonecore::Result<ProductLogCompletionReport>::failure(
        promoted.error());
  }
  report.promoted = true;
  return clonecore::Result<ProductLogCompletionReport>::success(
      std::move(report));
}

clonecore::Result<ProductLogCompletionReport>
complete_windows_product_log_session(
    const std::wstring& data_directory,
    const std::wstring& failed_log_path,
    const bool clean_close_requested,
    const bool error_detected,
    const bool active_write_operation) {
  try {
    const std::filesystem::path data(data_directory);
    const std::filesystem::path failed(failed_log_path);
    const std::filesystem::path normalized_data = data.lexically_normal();
    const std::filesystem::path normalized_failed =
        failed.lexically_normal();
    const std::filesystem::path log_directory =
        normalized_data / L"logs";
    if (!data.is_absolute() || !failed.is_absolute() ||
        _wcsicmp(normalized_data.filename().c_str(), L"data") != 0 ||
        _wcsicmp(
            normalized_data.native().c_str(), data.native().c_str()) != 0 ||
        _wcsicmp(
            normalized_failed.native().c_str(), failed.native().c_str()) != 0 ||
        _wcsicmp(
            normalized_failed.parent_path().native().c_str(),
            log_directory.native().c_str()) != 0) {
      return clonecore::Result<ProductLogCompletionReport>::failure(
          retention_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_NAME,
              L"製品ログ完了分類のパス確認",
              L"EXE隣data\\logsの直接のfailedログとして確認できません"));
    }

    const std::wstring failed_file_name =
        normalized_failed.filename().native();
    auto adjacent_data = inspect_windows_startup_data_backing();
    if (!adjacent_data || !adjacent_data.value().data_directory_exists ||
        _wcsicmp(
            std::filesystem::path(adjacent_data.value().data_directory)
                .lexically_normal()
                .native()
                .c_str(),
            normalized_data.native().c_str()) != 0) {
      return clonecore::Result<ProductLogCompletionReport>::failure(
          adjacent_data
              ? retention_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_INVALID_NAME,
                    L"製品ログ完了分類のEXE隣data再確認",
                    L"指定dataが現在のEXE隣dataと一致しません")
              : adjacent_data.error());
    }
    auto pinned_data = open_pinned_log_directory(
        normalized_data.native(), L"製品ログ完了分類のdata固定");
    if (!pinned_data) {
      return clonecore::Result<ProductLogCompletionReport>::failure(
          pinned_data.error());
    }
    WindowsProductLogCompletionPlatform platform;
    return complete_product_log_session(
        platform,
        log_directory.native(),
        failed_file_name,
        clean_close_requested,
        error_detected,
        active_write_operation);
  } catch (const std::bad_alloc&) {
    return clonecore::Result<ProductLogCompletionReport>::failure(
        retention_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"製品ログ完了分類",
            L"完了分類に必要なメモリを確保できません"));
  } catch (...) {
    return clonecore::Result<ProductLogCompletionReport>::failure(
        retention_error(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"製品ログ完了分類",
            L"完了分類を安全に完了できません"));
  }
}

clonecore::Result<ProductLogRetentionPlan> plan_product_log_retention(
    const std::vector<ProductLogObservation>& observations,
    const std::uint64_t now_utc_100ns) {
  ProductLogRetentionPlan plan{};
  std::set<std::wstring> owned_names;
  std::vector<ProductLogObservation> normal_candidates;

  for (const auto& observation : observations) {
    const auto log_class =
        classify_product_log_file_name(observation.file_name);
    if (!log_class.has_value() || !observation.regular_file ||
        observation.reparse_point || !observation.has_utf8_bom) {
      ++plan.ignored_count;
      continue;
    }
    if (!owned_names.insert(observation.file_name).second) {
      return clonecore::Result<ProductLogRetentionPlan>::failure(
          retention_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_DUP_NAME,
              L"製品ログ循環計画",
              L"同じ製品ログ名が複数回列挙されました"));
    }

    if (log_class.value() == ProductLogClass::failure) {
      if (retention_elapsed(
              observation.last_write_utc_100ns,
              now_utc_100ns,
              kProductLogFailureRetentionDays)) {
        plan.deletions.push_back(ProductLogDeletion{
            .observed = observation,
            .log_class = ProductLogClass::failure,
            .reason = ProductLogDeletionReason::failure_age,
        });
      } else {
        ++plan.retained_failure_count;
      }
      continue;
    }

    if (retention_elapsed(
            observation.last_write_utc_100ns,
            now_utc_100ns,
            kProductLogNormalRetentionDays)) {
      plan.deletions.push_back(ProductLogDeletion{
          .observed = observation,
          .log_class = ProductLogClass::normal,
          .reason = ProductLogDeletionReason::normal_age,
      });
      continue;
    }
    normal_candidates.push_back(observation);
  }

  std::sort(
      normal_candidates.begin(),
      normal_candidates.end(),
      observation_less);
  std::size_t first_retained = normal_candidates.size();
  std::uint64_t retained_bytes = 0U;
  for (std::size_t cursor = normal_candidates.size(); cursor > 0U; --cursor) {
    const auto& candidate = normal_candidates[cursor - 1U];
    if (candidate.size_bytes >
        kProductLogNormalBudgetBytes - retained_bytes) {
      break;
    }
    retained_bytes += candidate.size_bytes;
    first_retained = cursor - 1U;
  }
  for (std::size_t cursor = 0U; cursor < first_retained; ++cursor) {
    const auto& candidate = normal_candidates[cursor];
    plan.deletions.push_back(ProductLogDeletion{
        .observed = candidate,
        .log_class = ProductLogClass::normal,
        .reason = ProductLogDeletionReason::normal_budget,
    });
  }
  plan.retained_normal_bytes = retained_bytes;
  plan.retained_normal_count = normal_candidates.size() - first_retained;
  std::sort(plan.deletions.begin(), plan.deletions.end(), deletion_less);
  return clonecore::Result<ProductLogRetentionPlan>::success(
      std::move(plan));
}

clonecore::Result<ProductLogRetentionReport>
enforce_product_log_retention(
    IProductLogRetentionPlatform& platform,
    const std::wstring& log_directory,
    const std::uint64_t now_utc_100ns) {
  if (log_directory.empty()) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        retention_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"製品ログ循環",
            L"ログフォルダーが空です"));
  }
  auto observations = platform.enumerate_owned_candidates(log_directory);
  if (!observations) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        observations.error());
  }
  auto plan = plan_product_log_retention(
      observations.value(), now_utc_100ns);
  if (!plan) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        plan.error());
  }

  ProductLogRetentionReport report{
      .plan = plan.take_value(),
  };
  for (const auto& deletion : report.plan.deletions) {
    const auto removed = platform.delete_if_unchanged(
        log_directory, deletion.observed);
    if (!removed) {
      return clonecore::Result<ProductLogRetentionReport>::failure(
          removed.error());
    }
    ++report.deleted_count;
    report.deleted_bytes = saturating_add(
        report.deleted_bytes, deletion.observed.size_bytes);
  }
  return clonecore::Result<ProductLogRetentionReport>::success(
      std::move(report));
}

clonecore::Result<ProductLogRetentionReport>
enforce_windows_product_log_retention(
    const std::wstring& data_directory) {
  if (data_directory.empty()) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        retention_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"製品ログ循環",
            L"EXE隣dataフォルダーが空です"));
  }
  const std::filesystem::path data_path(data_directory);
  if (!data_path.is_absolute() ||
      _wcsicmp(data_path.filename().c_str(), L"data") != 0) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        retention_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"製品ログ循環",
            L"EXE隣dataフォルダーとして確認できません"));
  }
  const auto data_safe = require_regular_non_reparse_directory(
      data_directory, L"製品ログ循環前のdataフォルダー確認");
  if (!data_safe) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        data_safe.error());
  }
  const std::wstring log_directory =
      (data_path / L"logs").native();
  const auto logs_safe = require_regular_non_reparse_directory(
      log_directory, L"製品ログ循環前のlogsフォルダー確認");
  if (!logs_safe) {
    return clonecore::Result<ProductLogRetentionReport>::failure(
        logs_safe.error());
  }

  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const std::uint64_t now_utc_100ns =
      (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) |
      static_cast<std::uint64_t>(now.dwLowDateTime);
  WindowsProductLogRetentionPlatform platform;
  return enforce_product_log_retention(
      platform, log_directory, now_utc_100ns);
}

}  // namespace ytec::windowsapp

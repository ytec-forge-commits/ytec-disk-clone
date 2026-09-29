#include "ytec/imageformat/windows_tsumugi_rescue_staging.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::wstring_view kStagingSuffix = L".rescue-stage.partial";

clonecore::Error staging_error(
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

clonecore::Status staging_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(staging_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(
          left.data(),
          static_cast<int>(left.size()),
          right.data(),
          static_cast<int>(right.size()),
          TRUE) == CSTR_EQUAL;
}

bool all_zero(const Sha256Digest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

bool same_path_identity(
    const WindowsImagePathIdentity& left,
    const WindowsImagePathIdentity& right) noexcept {
  return left.volume_serial_number == right.volume_serial_number &&
      left.file_id == right.file_id && left.file_size == right.file_size;
}

clonecore::Status validate_request(
    const WindowsTsumugiRescueStagingRequest& request) {
  if (request.final_path.empty() || request.source_disk_size == 0U ||
      request.source_disk_size > static_cast<std::uint64_t>(
                                     (std::numeric_limits<LONGLONG>::max)()) ||
      (request.logical_sector_size != 512U &&
       request.logical_sector_size != 4096U) ||
      request.source_disk_size % request.logical_sector_size != 0U ||
      request.required_available_bytes <= request.source_disk_size ||
      all_zero(request.source_model_hash) ||
      all_zero(request.source_serial_hash) ||
      all_zero(request.source_state_hash)) {
    return staging_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi救出一時領域要求",
        L"保存先、Source寸法、sector、識別Hash、または一時領域と画像を合わせた必要容量が不正です");
  }
  return clonecore::validate_stable_identity(
      request.expected_source_disk,
      request.expected_source_disk,
      L"Tsumugi救出コピー元");
}

clonecore::Status validate_active_destination(
    const WindowsImageDestinationObservation& initial,
    const WindowsImageDestinationObservation& current,
    const std::uint64_t required_image_bytes,
    const std::uint64_t expected_owned_partial_bytes = 0U) {
  const bool before_commit = expected_owned_partial_bytes != 0U;
  const bool supported_file_system =
      current.file_system == WindowsImageDestinationFileSystem::ntfs ||
      current.file_system == WindowsImageDestinationFileSystem::exfat;
  if (!equals_ordinal_ignore_case(
          initial.canonical_final_path, current.canonical_final_path) ||
      !equals_ordinal_ignore_case(initial.partial_path, current.partial_path) ||
      current.parent_is_reparse || current.physical_disk_extent_count != 1U ||
      !supported_file_system ||
      current.partial_exists != before_commit ||
      !current.parent_identity.has_value() ||
      current.final_exists != current.final_identity.has_value() ||
      (!before_commit && current.available_bytes < required_image_bytes) ||
      (before_commit &&
       (!current.partial_identity.has_value() ||
        current.partial_identity->file_size !=
            expected_owned_partial_bytes))) {
    return staging_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi救出一時領域の保存先再確認",
        L"保存先パス、親、filesystem、空き容量、完成名、または画像.partialの状態が変化しました");
  }
  if (!initial.parent_identity.has_value() ||
      !same_path_identity(
          initial.parent_identity.value(), current.parent_identity.value()) ||
      initial.final_exists != current.final_exists ||
      (initial.final_exists &&
       (!initial.final_identity.has_value() ||
        !same_path_identity(
            initial.final_identity.value(), current.final_identity.value())))) {
    return staging_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi救出一時領域の親・完成名再識別",
        L"作業中に保存先の親または既存完成ファイルが置換されました");
  }
  return clonecore::validate_stable_identity(
      initial.destination_disk,
      current.destination_disk,
      L"Tsumugi救出一時領域の保存先Disk");
}

clonecore::Result<std::wstring> make_staging_path(
    const std::wstring& canonical_final_path) {
  if (canonical_final_path.size() + kStagingSuffix.size() >=
      kMaximumPathCharacters) {
    return clonecore::Result<std::wstring>::failure(staging_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILENAME_EXCED_RANGE,
        L"Tsumugi救出一時領域パス",
        L"完成パスから導出した一時領域パスがWindows上限を超えます"));
  }
  return clonecore::Result<std::wstring>::success(
      canonical_final_path + std::wstring(kStagingSuffix));
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

struct OwnedFileIdentity final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16> file_id{};
  std::uint64_t file_size{};
  std::uint64_t allocation_size{};
  std::uint32_t link_count{};
  LARGE_INTEGER last_write{};
  LARGE_INTEGER change_time{};
};

bool same_file_id(
    const OwnedFileIdentity& left,
    const OwnedFileIdentity& right) noexcept {
  return left.volume_serial == right.volume_serial &&
      left.file_id == right.file_id;
}

bool same_owned_file_observation(
    const OwnedFileIdentity& left,
    const OwnedFileIdentity& right) noexcept {
  return same_file_id(left, right) && left.file_size == right.file_size &&
      left.allocation_size == right.allocation_size &&
      left.link_count == right.link_count;
}

bool same_sealed_observation(
    const OwnedFileIdentity& left,
    const OwnedFileIdentity& right) noexcept {
  return same_file_id(left, right) && left.file_size == right.file_size &&
      left.allocation_size == right.allocation_size &&
      left.link_count == right.link_count &&
      left.last_write.QuadPart == right.last_write.QuadPart &&
      left.change_time.QuadPart == right.change_time.QuadPart;
}

clonecore::Result<OwnedFileIdentity> identity_from_handle(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_ID_INFO id{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !GetFileInformationByHandleEx(
          handle, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic))) {
    return clonecore::Result<OwnedFileIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            GetLastError()));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      standard.EndOfFile.QuadPart < 0 ||
      standard.AllocationSize.QuadPart < 0 || standard.NumberOfLinks != 1U) {
    return clonecore::Result<OwnedFileIdentity>::failure(staging_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の単一linkファイルとして識別できません"));
  }
  OwnedFileIdentity result{
      .volume_serial = id.VolumeSerialNumber,
      .file_size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .allocation_size =
          static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
      .link_count = standard.NumberOfLinks,
      .last_write = basic.LastWriteTime,
      .change_time = basic.ChangeTime,
  };
  static_assert(sizeof(id.FileId.Identifier) == 16U);
  std::memcpy(
      result.file_id.data(), id.FileId.Identifier, result.file_id.size());
  return clonecore::Result<OwnedFileIdentity>::success(result);
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
          std::wstring_view(actual.data(), length), extended_path(expected))) {
    return staging_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"所有ハンドルの実体パスが導出済み一時領域パスと一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status seek_file(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::wstring_view operation) {
  if (offset > static_cast<std::uint64_t>(
                   (std::numeric_limits<LONGLONG>::max)())) {
    return staging_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"ファイル位置がWindows上限を超えます");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Status set_file_length(
    const HANDLE handle,
    const std::uint64_t length) {
  auto status = seek_file(handle, length, L"Tsumugi救出一時領域長設定");
  if (!status) {
    return status;
  }
  if (!SetEndOfFile(handle)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Tsumugi救出一時領域長設定",
        GetLastError()));
  }
  return clonecore::success_status();
}

class LocalSecurityDescriptor final {
 public:
  LocalSecurityDescriptor() = default;
  ~LocalSecurityDescriptor() {
    if (value_ != nullptr) {
      LocalFree(value_);
    }
  }
  LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
  LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;

  [[nodiscard]] PSECURITY_DESCRIPTOR* put() noexcept { return &value_; }
  [[nodiscard]] PSECURITY_DESCRIPTOR get() const noexcept { return value_; }

 private:
  PSECURITY_DESCRIPTOR value_{};
};

class Win32RescueStagingBackend final
    : public IWindowsTsumugiRescueStagingBackend {
 public:
  ~Win32RescueStagingBackend() override {
    if (owns_) {
      static_cast<void>(discard_exact_owned_staging());
    }
  }

  [[nodiscard]] clonecore::Result<WindowsImageDestinationObservation>
  observe_destination(const std::wstring& final_path) override {
    return observe_windows_tsumugi_destination(final_path);
  }

  [[nodiscard]] clonecore::Status create_new_owned_staging(
      const std::wstring& staging_path,
      const std::uint64_t expected_length) override {
    if (owns_ || handle_ || expected_length == 0U) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Tsumugi救出一時領域CREATE_NEW",
          L"所有一時領域は一度だけ作成できます");
    }
    LocalSecurityDescriptor descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)",
            SDDL_REVISION_1,
            descriptor.put(),
            nullptr)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::access_denied,
          L"Tsumugi救出一時領域ACL作成",
          GetLastError()));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.get();
    attributes.bInheritHandle = FALSE;
    handle_.reset(CreateFileW(
        extended_path(staging_path).c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr));
    if (!handle_) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Tsumugi救出一時領域CREATE_NEW",
          GetLastError()));
    }
    owns_ = true;
    staging_path_ = staging_path;

    auto status = verify_opened_path(
        handle_.get(), staging_path_, L"Tsumugi救出一時領域作成直後パス識別");
    auto identity = identity_from_handle(
        handle_.get(), L"Tsumugi救出一時領域作成直後識別");
    if (!status || !identity) {
      const clonecore::Error primary =
          status ? identity.error() : status.error();
      return fail_created_file(primary);
    }
    owned_identity_ = identity.value();
    status = set_file_length(handle_.get(), expected_length);
    if (!status) {
      return fail_created_file(status.error());
    }
    if (!FlushFileBuffers(handle_.get())) {
      return fail_created_file(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Tsumugi救出一時領域初期容量flush",
          GetLastError()));
    }
    identity = identity_from_handle(
        handle_.get(), L"Tsumugi救出一時領域初期容量識別");
    if (!identity || identity.value().file_size != expected_length ||
        !same_file_id(owned_identity_, identity.value())) {
      const clonecore::Error primary = identity
          ? staging_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_FILE_INVALID,
                L"Tsumugi救出一時領域初期容量検証",
                L"作成した一時領域のFile IDまたは長さが一致しません")
          : identity.error();
      return fail_created_file(primary);
    }
    owned_identity_ = identity.value();
    expected_length_ = expected_length;
    return clonecore::success_status();
  }

  [[nodiscard]] bool owns_staging() const noexcept override {
    return owns_;
  }

  [[nodiscard]] bool sealed_for_read() const noexcept override {
    return sealed_;
  }

  [[nodiscard]] clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (!owns_ || sealed_ || !handle_) {
      return staging_failure(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_STATE,
          L"Tsumugi救出一時領域write",
          L"書込み可能な所有ハンドルがありません");
    }
    auto status = seek_file(handle_.get(), offset, L"Tsumugi救出一時領域write");
    if (!status) {
      return status;
    }
    std::size_t completed = 0U;
    while (completed < bytes.size()) {
      const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - completed,
          (std::numeric_limits<DWORD>::max)()));
      DWORD written = 0U;
      if (!WriteFile(
              handle_.get(),
              bytes.data() + completed,
              amount,
              &written,
              nullptr) ||
          written != amount) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Tsumugi救出一時領域write",
            written == amount ? GetLastError() : ERROR_WRITE_FAULT));
      }
      completed += written;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!owns_ || !handle_) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          staging_error(
              clonecore::ErrorCode::access_denied,
              ERROR_INVALID_STATE,
              L"Tsumugi救出一時領域read",
              L"読取り可能な所有ハンドルがありません"));
    }
    auto status = seek_file(handle_.get(), offset, L"Tsumugi救出一時領域read");
    if (!status) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          status.error());
    }
    std::vector<std::byte> bytes(length);
    std::size_t completed = 0U;
    while (completed < bytes.size()) {
      const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - completed,
          (std::numeric_limits<DWORD>::max)()));
      DWORD read = 0U;
      if (!ReadFile(
              handle_.get(),
              bytes.data() + completed,
              amount,
              &read,
              nullptr)) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"Tsumugi救出一時領域read",
                GetLastError()));
      }
      if (read != amount) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            staging_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_HANDLE_EOF,
                L"Tsumugi救出一時領域read",
                L"所有ファイルが要求範囲の途中で終了しました"));
      }
      completed += read;
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  [[nodiscard]] clonecore::Status flush() override {
    if (!owns_ || sealed_ || !handle_ ||
        !FlushFileBuffers(handle_.get())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Tsumugi救出一時領域flush",
          handle_ ? GetLastError() : ERROR_INVALID_HANDLE));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status seal_read_only(
      const std::wstring& staging_path,
      const std::uint64_t expected_length) override {
    if (!owns_ || sealed_ || !handle_ ||
        !equals_ordinal_ignore_case(staging_path_, staging_path) ||
        expected_length_ != expected_length) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Tsumugi救出一時領域封印",
          L"所有状態、導出パス、または予定長が一致しません");
    }
    if (!FlushFileBuffers(handle_.get())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Tsumugi救出一時領域封印前flush",
          GetLastError()));
    }
    auto before = identity_from_handle(
        handle_.get(), L"Tsumugi救出一時領域封印前識別");
    auto path_status = verify_opened_path(
        handle_.get(), staging_path_, L"Tsumugi救出一時領域封印前パス識別");
    if (!before || !path_status || before.value().file_size != expected_length ||
        !same_file_id(owned_identity_, before.value())) {
      return before
          ? (path_status
                 ? staging_failure(
                       clonecore::ErrorCode::identity_mismatch,
                       ERROR_FILE_INVALID,
                       L"Tsumugi救出一時領域封印前識別",
                       L"所有File IDまたは予定長が変化しました")
                 : path_status)
          : clonecore::Status::failure(before.error());
    }
    handle_.reset();
    clonecore::UniqueHandle read_only(CreateFileW(
        extended_path(staging_path_).c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_RANDOM_ACCESS,
        nullptr));
    if (!read_only) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::identity_mismatch,
          L"Tsumugi救出一時領域read-only再open",
          GetLastError()));
    }
    auto after = identity_from_handle(
        read_only.get(), L"Tsumugi救出一時領域read-only再識別");
    path_status = verify_opened_path(
        read_only.get(), staging_path_, L"Tsumugi救出一時領域read-onlyパス再識別");
    if (after && same_file_id(owned_identity_, after.value())) {
      handle_ = std::move(read_only);
    }
    if (!after || !path_status || !handle_ ||
        !same_owned_file_observation(before.value(), after.value()) ||
        after.value().file_size != expected_length) {
      return after
          ? (path_status
                 ? staging_failure(
                       clonecore::ErrorCode::identity_mismatch,
                       ERROR_FILE_INVALID,
                       L"Tsumugi救出一時領域read-only再識別",
                       L"read-only再open時にFile ID、長さ、link数、または割当量が変化しました")
                  : path_status)
          : clonecore::Status::failure(after.error());
    }
    auto confirmed = identity_from_handle(
        handle_.get(), L"Tsumugi救出一時領域read-only封印再確認");
    if (!confirmed ||
        !same_sealed_observation(after.value(), confirmed.value())) {
      return confirmed
          ? staging_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Tsumugi救出一時領域read-only封印再確認",
                L"read-only封印後にFile ID、長さ、link数、時刻、または割当量が安定しません")
          : clonecore::Status::failure(confirmed.error());
    }
    owned_identity_ = confirmed.value();
    sealed_ = true;
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status discard_exact_owned_staging() override {
    if (!owns_) {
      return clonecore::success_status();
    }
    if (!handle_) {
      handle_.reset(CreateFileW(
          extended_path(staging_path_).c_str(),
          GENERIC_READ | DELETE,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr));
      if (!handle_) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::identity_mismatch,
            L"Tsumugi救出一時領域破棄前再open",
            GetLastError()));
      }
    }
    auto identity = identity_from_handle(
        handle_.get(), L"Tsumugi救出一時領域破棄前識別");
    if (!identity || !same_file_id(owned_identity_, identity.value())) {
      return identity
          ? staging_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"Tsumugi救出一時領域破棄前識別",
                L"所有File IDと一致しないため削除しません")
          : clonecore::Status::failure(identity.error());
    }
    return delete_owned_handle();
  }

 private:
  [[nodiscard]] clonecore::Status delete_owned_handle() {
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    if (!SetFileInformationByHandle(
            handle_.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition))) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"Tsumugi救出一時領域handle破棄",
          GetLastError()));
    }
    handle_.reset();
    owns_ = false;
    sealed_ = false;
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status fail_created_file(
      const clonecore::Error& primary) {
    const auto cleanup = delete_owned_handle();
    if (!cleanup) {
      return staging_failure(
          cleanup.error().code,
          cleanup.error().native_code,
          L"Tsumugi救出一時領域作成失敗後破棄",
          primary.message + L" / " + cleanup.error().message);
    }
    return clonecore::Status::failure(primary);
  }

  std::wstring staging_path_;
  clonecore::UniqueHandle handle_;
  OwnedFileIdentity owned_identity_{};
  std::uint64_t expected_length_{};
  bool owns_{};
  bool sealed_{};
};

class WindowsTsumugiRescueStagingSession final
    : public ITsumugiRescueStagingSession {
 public:
  WindowsTsumugiRescueStagingSession(
      WindowsTsumugiRescueStagingRequest request,
      WindowsImageDestinationObservation initial,
      std::wstring staging_path,
      std::uint64_t required_image_bytes,
      std::unique_ptr<IWindowsTsumugiRescueStagingBackend> backend)
      : request_(std::move(request)),
        initial_(std::move(initial)),
        staging_path_(std::move(staging_path)),
        required_image_bytes_(required_image_bytes),
        backend_(std::move(backend)) {}

  ~WindowsTsumugiRescueStagingSession() override {
    if (!discarded_) {
      static_cast<void>(discard_owned_staging());
    }
  }

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return request_.source_disk_size;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return request_.logical_sector_size;
  }

  [[nodiscard]] Sha256Digest source_model_hash() const noexcept override {
    return request_.source_model_hash;
  }

  [[nodiscard]] Sha256Digest source_serial_hash() const noexcept override {
    return request_.source_serial_hash;
  }

  [[nodiscard]] Sha256Digest source_state_hash() const noexcept override {
    return request_.source_state_hash;
  }

  [[nodiscard]] clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    auto status = validate_range(offset, bytes.size(), false);
    if (!status) {
      return status;
    }
    status = backend_->write_at(offset, bytes);
    if (status) {
      flushed_ = false;
    }
    return status;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    auto status = validate_range(offset, length, false);
    if (!status) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          status.error());
    }
    return backend_->read_at(offset, length);
  }

  [[nodiscard]] clonecore::Status flush_target() override {
    auto status = validate_writable(L"Tsumugi救出一時領域flush");
    if (!status) {
      return status;
    }
    status = backend_->flush();
    if (status) {
      flushed_ = true;
    }
    return status;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    auto status = validate_range(offset, length, true);
    if (!status) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          status.error());
    }
    return backend_->read_at(offset, length);
  }

  [[nodiscard]] clonecore::Status seal_for_image_read() override {
    if (!flushed_ || sealed_ || discarded_ ||
        backend_->sealed_for_read() || !backend_->owns_staging()) {
      return staging_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"Tsumugi救出一時領域封印",
          L"完全flush済みの未封印所有一時領域だけを封印できます");
    }
    auto observation = backend_->observe_destination(request_.final_path);
    if (!observation) {
      return clonecore::Status::failure(observation.error());
    }
    auto status = validate_active_destination(
        initial_, observation.value(), required_image_bytes_);
    if (!status) {
      return status;
    }
    status = backend_->seal_read_only(
        staging_path_, request_.source_disk_size);
    if (!status) {
      return status;
    }
    observation = backend_->observe_destination(request_.final_path);
    if (!observation) {
      return clonecore::Status::failure(observation.error());
    }
    status = validate_active_destination(
        initial_, observation.value(), required_image_bytes_);
    if (!status) {
      return status;
    }
    if (!backend_->sealed_for_read()) {
      return staging_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"Tsumugi救出一時領域封印",
          L"backendがread-only封印を証明しませんでした");
    }
    sealed_ = true;
    return clonecore::success_status();
  }

  [[nodiscard]] bool sealed_for_image_read() const noexcept override {
    return sealed_ && !discarded_ && backend_->sealed_for_read();
  }

  [[nodiscard]] clonecore::Status discard_owned_staging() noexcept override {
    if (discarded_) {
      return clonecore::success_status();
    }
    try {
      const auto status = backend_->discard_exact_owned_staging();
      if (status) {
        was_sealed_ = sealed_ || backend_->sealed_for_read();
        discarded_ = true;
        sealed_ = false;
        flushed_ = false;
      }
      return status;
    } catch (...) {
      return staging_failure(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Tsumugi救出一時領域破棄",
          L"所有一時領域の破棄中に予期しない例外が発生しました");
    }
  }

  [[nodiscard]] clonecore::Status
  validate_image_destination_before_commit(
      const std::uint64_t expected_owned_partial_bytes) override {
    if (!discarded_ || !was_sealed_ || expected_owned_partial_bytes == 0U ||
        backend_->owns_staging()) {
      return staging_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"Tsumugi救出画像完成前保存先再識別",
          L"封印済み一時領域の完全破棄と画像.partial長が必要です");
    }
    auto observation = backend_->observe_destination(request_.final_path);
    if (!observation) {
      return clonecore::Status::failure(observation.error());
    }
    return validate_active_destination(
        initial_,
        observation.value(),
        0U,
        expected_owned_partial_bytes);
  }

 private:
  [[nodiscard]] clonecore::Status validate_writable(
      const std::wstring_view operation) const {
    if (discarded_ || sealed_ || backend_->sealed_for_read() ||
        !backend_->owns_staging()) {
      return staging_failure(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_STATE,
          std::wstring(operation),
          L"書込み可能な所有一時領域ではありません");
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status validate_range(
      const std::uint64_t offset,
      const std::size_t length,
      const bool require_sealed) const {
    if (length == 0U || offset > request_.source_disk_size ||
        static_cast<std::uint64_t>(length) >
            request_.source_disk_size - offset) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Tsumugi救出一時領域I/O範囲",
          L"要求範囲が所有一時領域の境界外です");
    }
    if (require_sealed) {
      if (!sealed_for_image_read()) {
        return staging_failure(
            clonecore::ErrorCode::access_denied,
            ERROR_INVALID_STATE,
            L"Tsumugi救出一時領域image read",
            L"画像Sourceとしてはread-only封印後だけ読取れます");
      }
      return clonecore::success_status();
    }
    return validate_writable(L"Tsumugi救出一時領域read-back");
  }

  WindowsTsumugiRescueStagingRequest request_;
  WindowsImageDestinationObservation initial_;
  std::wstring staging_path_;
  std::uint64_t required_image_bytes_{};
  std::unique_ptr<IWindowsTsumugiRescueStagingBackend> backend_;
  bool flushed_{};
  bool sealed_{};
  bool was_sealed_{};
  bool discarded_{};
};

clonecore::Result<std::unique_ptr<ITsumugiRescueStagingSession>>
failure_after_cleanup(
    IWindowsTsumugiRescueStagingBackend& backend,
    clonecore::Error primary) {
  if (!backend.owns_staging()) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        std::move(primary));
  }
  const auto cleanup = backend.discard_exact_owned_staging();
  if (!cleanup) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        staging_error(
            cleanup.error().code,
            cleanup.error().native_code,
            L"Tsumugi救出一時領域開始失敗後破棄",
            primary.message + L" / " + cleanup.error().message));
  }
  return clonecore::Result<
      std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
      std::move(primary));
}

}  // namespace

clonecore::Result<std::unique_ptr<ITsumugiRescueStagingSession>>
make_windows_tsumugi_rescue_staging_session(
    const WindowsTsumugiRescueStagingRequest& request) {
  return make_windows_tsumugi_rescue_staging_session_with_backend(
      request, std::make_unique<Win32RescueStagingBackend>());
}

clonecore::Result<std::unique_ptr<ITsumugiRescueStagingSession>>
make_windows_tsumugi_rescue_staging_session_with_backend(
    const WindowsTsumugiRescueStagingRequest& request,
    std::unique_ptr<IWindowsTsumugiRescueStagingBackend> backend) {
  if (!backend) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        staging_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"Tsumugi救出一時領域backend",
            L"backendがありません"));
  }
  auto status = validate_request(request);
  if (!status) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        status.error());
  }
  auto initial = backend->observe_destination(request.final_path);
  if (!initial) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        initial.error());
  }
  const WindowsTsumugiDestinationGuardRequest initial_guard{
      .final_path = request.final_path,
      .expected_source_disk = request.expected_source_disk,
      .required_available_bytes = request.required_available_bytes,
      .replace_existing = request.replace_existing,
      .phase = WindowsTsumugiDestinationGuardPhase::before_stage,
  };
  status = validate_windows_tsumugi_destination_observation(
      initial_guard, initial.value());
  if (!status) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        status.error());
  }
  auto staging_path = make_staging_path(
      initial.value().canonical_final_path);
  if (!staging_path) {
    return clonecore::Result<
        std::unique_ptr<ITsumugiRescueStagingSession>>::failure(
        staging_path.error());
  }
  status = backend->create_new_owned_staging(
      staging_path.value(), request.source_disk_size);
  if (!status) {
    return failure_after_cleanup(*backend, status.error());
  }
  const std::uint64_t required_image_bytes =
      request.required_available_bytes - request.source_disk_size;
  auto current = backend->observe_destination(request.final_path);
  if (!current) {
    return failure_after_cleanup(*backend, current.error());
  }
  WindowsTsumugiDestinationGuardRequest current_guard = initial_guard;
  current_guard.required_available_bytes = required_image_bytes;
  status = validate_windows_tsumugi_destination_observation(
      current_guard, current.value());
  if (!status) {
    return failure_after_cleanup(*backend, status.error());
  }
  status = validate_active_destination(
      initial.value(), current.value(), required_image_bytes);
  if (!status) {
    return failure_after_cleanup(*backend, status.error());
  }
  return clonecore::Result<
      std::unique_ptr<ITsumugiRescueStagingSession>>::success(
      std::make_unique<WindowsTsumugiRescueStagingSession>(
          request,
          initial.take_value(),
          staging_path.take_value(),
          required_image_bytes,
          std::move(backend)));
}

}  // namespace ytec::imageformat

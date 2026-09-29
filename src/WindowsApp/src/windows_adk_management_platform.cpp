#include "ytec/windowsapp/windows_adk_management_platform.h"

#include "ytec/clonecore/error.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/sha256.h"

#include <Windows.h>
#include <Msi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kIoBlockBytes = 1024U * 1024U;
constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;

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

template <typename T>
clonecore::Result<T> failure(
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

clonecore::Status status_failure(
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

struct FileIdentity final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};

  friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

struct FileObservation final {
  FileIdentity identity;
  std::uint64_t byte_count{};
  std::uint32_t link_count{};
  bool directory{};
  bool reparse_point{};
};

clonecore::Result<FileObservation> observe_handle(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ID_INFO id{};
  FILE_STANDARD_INFO standard{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (GetFileInformationByHandleEx(
          handle, FileIdInfo, &id, sizeof(id)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE ||
      GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  FileObservation observation{
      .identity = FileIdentity{
          .volume_serial = id.VolumeSerialNumber,
      },
      .byte_count = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .link_count = standard.NumberOfLinks,
      .directory = standard.Directory != FALSE,
      .reparse_point =
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U,
  };
  for (std::size_t index = 0U;
       index < observation.identity.file_id.size();
       ++index) {
    observation.identity.file_id[index] =
        static_cast<std::byte>(id.FileId.Identifier[index]);
  }
  return clonecore::Result<FileObservation>::success(observation);
}

bool regular_single_non_reparse(
    const FileObservation& observation) noexcept {
  return !observation.directory && !observation.reparse_point &&
         observation.link_count == 1U;
}

bool local_absolute_path(const std::filesystem::path& path) {
  const std::wstring value = path.native();
  return value.size() >= 3U && value.size() < kMaximumPathCharacters &&
         ((value[0] >= L'A' && value[0] <= L'Z') ||
          (value[0] >= L'a' && value[0] <= L'z')) &&
         value[1] == L':' &&
         (value[2] == L'\\' || value[2] == L'/') &&
         !value.starts_with(L"\\\\") && !value.starts_with(L"\\?") &&
         !value.starts_with(L"\\.");
}

bool safe_leaf(const std::filesystem::path& path) {
  const std::wstring value = path.native();
  if (value.empty() || value.size() > 128U || path != path.filename() ||
      value == L"." || value == L".." || value.back() == L'.' ||
      value.back() == L' ' ||
      value.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
      std::any_of(value.begin(), value.end(), [](const wchar_t character) {
        return character < 0x20;
      })) {
    return false;
  }
  return true;
}

clonecore::Result<std::filesystem::path> full_local_path(
    const std::filesystem::path& path,
    const std::wstring_view operation) {
  if (!local_absolute_path(path)) {
    return failure<std::filesystem::path>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ローカルドライブの絶対パスではありません");
  }
  std::vector<wchar_t> buffer(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::filesystem::path>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            length == 0U ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
  }
  std::filesystem::path result(
      std::wstring(buffer.data(), static_cast<std::size_t>(length)));
  if (!local_absolute_path(result)) {
    return failure<std::filesystem::path>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"正規化後のパスがローカル境界外です");
  }
  return clonecore::Result<std::filesystem::path>::success(
      std::move(result));
}

clonecore::Result<std::filesystem::path> module_directory() {
  std::vector<wchar_t> buffer(kMaximumPathCharacters, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::filesystem::path>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ADK管理記録 EXEパス取得",
            length == 0U ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
  }
  return full_local_path(
      std::filesystem::path(
          std::wstring(buffer.data(), static_cast<std::size_t>(length)))
          .parent_path(),
      L"ADK管理記録 EXE配置先正規化");
}

clonecore::Result<clonecore::UniqueHandle> open_normal_directory(
    const std::filesystem::path& path,
    const std::wstring_view operation,
    const DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE,
    const DWORD desired_access = FILE_READ_ATTRIBUTES) {
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      desired_access,
      share_mode,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed, operation, GetLastError()));
  }
  const auto observed = observe_handle(handle.get(), operation);
  if (!observed || !observed.value().directory ||
      observed.value().reparse_point) {
    return failure<clonecore::UniqueHandle>(
        clonecore::ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparseディレクトリではありません");
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      std::move(handle));
}

clonecore::Result<std::filesystem::path> managed_record_path() {
  const auto module = module_directory();
  if (!module) {
    return clonecore::Result<std::filesystem::path>::failure(module.error());
  }
  const auto data = module.value() / L"data";
  const auto opened = open_normal_directory(
      data, L"ADK管理記録 EXE隣data固定");
  if (!opened) {
    return clonecore::Result<std::filesystem::path>::failure(opened.error());
  }
  return clonecore::Result<std::filesystem::path>::success(
      data / std::wstring(kManagedAdkRecordFileName));
}

clonecore::Result<std::vector<std::byte>> read_bounded_handle(
    const HANDLE handle,
    const std::uint64_t byte_count,
    const std::size_t maximum_bytes,
    const std::wstring_view operation) {
  if (byte_count == 0U || byte_count > maximum_bytes) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        std::wstring(operation),
        L"固定上限内の非空ファイルではありません");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(byte_count));
  LARGE_INTEGER beginning{};
  if (SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == FALSE) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            operation,
            GetLastError()));
  }
  std::size_t offset{};
  while (offset < bytes.size()) {
    const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, kIoBlockBytes));
    DWORD transferred{};
    if (ReadFile(
            handle,
            bytes.data() + offset,
            amount,
            &transferred,
            nullptr) == FALSE ||
        transferred != amount) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              operation,
              GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF
                                              : GetLastError()));
    }
    offset += transferred;
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

clonecore::Result<imageformat::Sha256Digest> hash_handle(
    const HANDLE handle,
    const std::uint64_t byte_count,
    const std::wstring_view operation) {
  return imageformat::sha256_from_reader(
      byte_count,
      kIoBlockBytes,
      [handle, operation](
          const std::uint64_t offset,
          const std::size_t length) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(
                handle, position, nullptr, FILE_BEGIN) == FALSE) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::io_failed,
                  operation,
                  GetLastError()));
        }
        std::vector<std::byte> block(length);
        DWORD transferred{};
        if (ReadFile(
                handle,
                block.data(),
                static_cast<DWORD>(block.size()),
                &transferred,
                nullptr) == FALSE ||
            transferred != block.size()) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::io_failed,
                  operation,
                  GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF
                                                  : GetLastError()));
        }
        return clonecore::Result<std::vector<std::byte>>::success(
            std::move(block));
      });
}

std::string digest_hex(const imageformat::Sha256Digest& digest) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(hex[(byte >> 4U) & 0x0FU]);
    result.push_back(hex[byte & 0x0FU]);
  }
  return result;
}

clonecore::Status write_all(
    const HANDLE handle,
    const std::span<const std::byte> bytes,
    const std::wstring_view operation) {
  std::size_t offset{};
  while (offset < bytes.size()) {
    const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, kIoBlockBytes));
    DWORD transferred{};
    if (WriteFile(
            handle,
            bytes.data() + offset,
            amount,
            &transferred,
            nullptr) == FALSE ||
        transferred != amount) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          operation,
          GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT
                                          : GetLastError()));
    }
    offset += transferred;
  }
  return clonecore::success_status();
}

clonecore::Status mark_delete(const HANDLE handle) {
  FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
  if (SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition)) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"ADK管理記録 handle固定削除",
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Status rename_non_overwriting_handle(
    const HANDLE handle,
    const std::filesystem::path& final_path) {
  const std::wstring value = final_path.native();
  if (!local_absolute_path(final_path) ||
      value.size() >
          static_cast<std::size_t>(
              (std::numeric_limits<DWORD>::max)()) / sizeof(wchar_t)) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"ADK管理記録 handle固定publish",
        L"完成先が有界ローカル絶対パスではありません");
  }
  const std::size_t name_bytes = value.size() * sizeof(wchar_t);
  constexpr std::size_t fixed_bytes = offsetof(FILE_RENAME_INFO, FileName);
  std::vector<std::byte> buffer(
      fixed_bytes + name_bytes + sizeof(wchar_t), std::byte{0});
  auto* const rename =
      reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(name_bytes);
  std::memcpy(rename->FileName, value.data(), name_bytes);
  if (SetFileInformationByHandle(
          handle,
          FileRenameInfo,
          buffer.data(),
          static_cast<DWORD>(buffer.size())) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"ADK管理記録 handle固定非上書きpublish",
        GetLastError()));
  }
  return clonecore::success_status();
}

bool record_equal(
    const AdkManagedInstallationRecord& left,
    const AdkManagedInstallationRecord& right) {
  return left.manifest_id == right.manifest_id &&
         left.installed_by_tsumugi == right.installed_by_tsumugi &&
         left.installed_registration_ids ==
             right.installed_registration_ids;
}

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'A' && value <= L'F') ||
         (value >= L'a' && value <= L'f');
}

bool strict_guid(const std::wstring_view value) noexcept {
  if (value.size() != 38U || value.front() != L'{' ||
      value.back() != L'}') {
    return false;
  }
  constexpr std::array<std::size_t, 4U> hyphens{9U, 14U, 19U, 24U};
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    const bool hyphen = std::find(
                            hyphens.begin(), hyphens.end(), index) !=
                        hyphens.end();
    if ((hyphen && value[index] != L'-') ||
        (!hyphen && !is_hex(value[index]))) {
      return false;
    }
  }
  return true;
}

struct UninstallOperation final {
  bool patch{};
  std::wstring first_guid;
  std::wstring product_guid;
};

clonecore::Result<UninstallOperation> parse_uninstall_registration(
    const AdkUninstallStep& step) {
  constexpr std::wstring_view msi_prefix = L"MSI|";
  constexpr std::wstring_view msp_prefix = L"MSP|";
  if (step.registration_id.starts_with(msi_prefix)) {
    const std::wstring guid =
        step.registration_id.substr(msi_prefix.size());
    if (!strict_guid(guid)) {
      return failure<UninstallOperation>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理対象 MSI登録識別子",
          L"固定MSI product GUID形式ではありません");
    }
    return clonecore::Result<UninstallOperation>::success(
        UninstallOperation{.first_guid = guid});
  }
  if (step.registration_id.starts_with(msp_prefix)) {
    const std::wstring value =
        step.registration_id.substr(msp_prefix.size());
    const auto separator = value.find(L'|');
    if (separator == std::wstring::npos ||
        value.find(L'|', separator + 1U) != std::wstring::npos) {
      return failure<UninstallOperation>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理対象 MSP登録識別子",
          L"固定patch/product GUID対ではありません");
    }
    const std::wstring patch = value.substr(0U, separator);
    const std::wstring product = value.substr(separator + 1U);
    if (!strict_guid(patch) || !strict_guid(product)) {
      return failure<UninstallOperation>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理対象 MSP登録識別子",
          L"固定patch/product GUID対が不正です");
    }
    return clonecore::Result<UninstallOperation>::success(
        UninstallOperation{
            .patch = true,
            .first_guid = patch,
            .product_guid = product,
        });
  }
  return failure<UninstallOperation>(
      clonecore::ErrorCode::invalid_data,
      ERROR_NOT_SUPPORTED,
      L"ADK管理対象 登録識別子",
      L"監査済みMSI|{GUID}またはMSP|{PATCH}|{PRODUCT}形式ではありません");
}

bool msi_product_present(const std::wstring& product_guid) {
  DWORD characters{};
  const UINT status = MsiGetProductInfoW(
      product_guid.c_str(),
      INSTALLPROPERTY_VERSIONSTRING,
      nullptr,
      &characters);
  return status == ERROR_MORE_DATA && characters > 0U;
}

}  // namespace

class WindowsAdkManagementPlatform::Impl final {
 public:
  ~Impl() { static_cast<void>(abandon_offline_layout()); }

  [[nodiscard]] clonecore::Result<
      std::optional<AdkManagedInstallationRecord>>
  load_managed_installation_record() {
    const auto path = managed_record_path();
    if (!path) {
      return clonecore::Result<
          std::optional<AdkManagedInstallationRecord>>::failure(
          path.error());
    }
    clonecore::UniqueHandle handle(CreateFileW(
        path.value().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND) {
        return clonecore::Result<
            std::optional<AdkManagedInstallationRecord>>::success(
            std::nullopt);
      }
      return clonecore::Result<
          std::optional<AdkManagedInstallationRecord>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADK管理記録 読取り固定",
              error));
    }
    const auto observed = observe_handle(
        handle.get(), L"ADK管理記録 読取り識別");
    if (!observed || !regular_single_non_reparse(observed.value())) {
      return failure<std::optional<AdkManagedInstallationRecord>>(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ADK管理記録 読取り識別",
          L"通常の非reparse単一リンクファイルではありません");
    }
    const auto bytes = read_bounded_handle(
        handle.get(),
        observed.value().byte_count,
        kMaximumManagedAdkRecordBytes,
        L"ADK管理記録 有界読取り");
    if (!bytes) {
      return clonecore::Result<
          std::optional<AdkManagedInstallationRecord>>::failure(
          bytes.error());
    }
    auto parsed = parse_managed_adk_record(bytes.value());
    if (!parsed) {
      return clonecore::Result<
          std::optional<AdkManagedInstallationRecord>>::failure(
          parsed.error());
    }
    return clonecore::Result<
        std::optional<AdkManagedInstallationRecord>>::success(
        parsed.take_value());
  }

  [[nodiscard]] clonecore::Status
  save_managed_installation_record_create_new(
      const AdkManagedInstallationRecord& record) {
    const auto bytes = serialize_managed_adk_record(record);
    if (!bytes) {
      return clonecore::Status::failure(bytes.error());
    }
    const auto final_path = managed_record_path();
    if (!final_path) {
      return clonecore::Status::failure(final_path.error());
    }
    const std::filesystem::path temporary =
        final_path.value().parent_path() /
        (L"." + final_path.value().filename().native() + L"." +
         std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(GetTickCount64()) + L".partial");
    clonecore::UniqueHandle handle(CreateFileW(
        temporary.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADK管理記録 CREATE_NEW stage",
          GetLastError()));
    }
    const auto observed = observe_handle(
        handle.get(), L"ADK管理記録 stage識別");
    if (!observed || !regular_single_non_reparse(observed.value()) ||
        observed.value().byte_count != 0U) {
      static_cast<void>(mark_delete(handle.get()));
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK管理記録 stage識別",
          L"空の通常非reparse単一リンクCREATE_NEW stageではありません");
    }
    const auto written = write_all(
        handle.get(), bytes.value(), L"ADK管理記録 stage書込み");
    if (!written || FlushFileBuffers(handle.get()) == FALSE) {
      const auto error = written
          ? clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"ADK管理記録 stage flush",
                GetLastError())
          : written.error();
      static_cast<void>(mark_delete(handle.get()));
      return clonecore::Status::failure(error);
    }
    const auto published = rename_non_overwriting_handle(
        handle.get(), final_path.value());
    if (!published) {
      const auto cleanup = mark_delete(handle.get());
      return clonecore::Status::failure(
          cleanup ? published.error() : cleanup.error());
    }
    handle.reset();
    const auto loaded = load_managed_installation_record();
    if (!loaded || !loaded.value().has_value() ||
        !record_equal(loaded.value().value(), record)) {
      return loaded
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"ADK管理記録 publish読戻し",
                L"保存後の固定schemaが導入証跡と一致しません")
          : clonecore::Status::failure(loaded.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status
  remove_managed_installation_record_if_exact(
      const AdkManagedInstallationRecord& record) {
    const auto path = managed_record_path();
    if (!path) {
      return clonecore::Status::failure(path.error());
    }
    clonecore::UniqueHandle handle(CreateFileW(
        path.value().c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADK管理記録 削除前固定",
          GetLastError()));
    }
    const auto observed = observe_handle(
        handle.get(), L"ADK管理記録 削除前識別");
    if (!observed || !regular_single_non_reparse(observed.value())) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ADK管理記録 削除前識別",
          L"通常の非reparse単一リンク管理記録ではありません");
    }
    const auto bytes = read_bounded_handle(
        handle.get(),
        observed.value().byte_count,
        kMaximumManagedAdkRecordBytes,
        L"ADK管理記録 削除前読取り");
    if (!bytes) {
      return clonecore::Status::failure(bytes.error());
    }
    const auto parsed = parse_managed_adk_record(bytes.value());
    if (!parsed) {
      return clonecore::Status::failure(parsed.error());
    }
    if (!record_equal(parsed.value(), record)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK管理記録 削除前一致確認",
          L"現在の管理記録がレビュー済みの導入証跡と一致しません");
    }
    return mark_delete(handle.get());
  }

  [[nodiscard]] clonecore::Status begin_new_offline_layout(
      const std::filesystem::path& layout_root,
      const std::string_view manifest_id) {
    if (layout_handle_ || layout_finalized_ || !published_files_.empty() ||
        manifest_id.empty() || manifest_id.size() > 128U) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"ADKオフラインレイアウト開始",
          L"同じadapterで複数layoutを開始できません");
    }
    const auto canonical = full_local_path(
        layout_root, L"ADKオフラインレイアウト保存先正規化");
    if (!canonical || canonical.value().root_path() == canonical.value()) {
      return canonical
          ? status_failure(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_NAME,
                L"ADKオフラインレイアウト保存先",
                L"ドライブルートは保存先にできません")
          : clonecore::Status::failure(canonical.error());
    }
    const auto parent = open_normal_directory(
        canonical.value().parent_path(),
        L"ADKオフラインレイアウト親固定");
    if (!parent) {
      return clonecore::Status::failure(parent.error());
    }
    if (CreateDirectoryW(canonical.value().c_str(), nullptr) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADKオフラインレイアウト CREATE_NEW directory",
          GetLastError()));
    }
    auto opened = open_normal_directory(
        canonical.value(),
        L"ADKオフラインレイアウト 新規directory固定",
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_READ_ATTRIBUTES | DELETE);
    if (!opened) {
      return clonecore::Status::failure(opened.error());
    }
    const auto observed = observe_handle(
        opened.value().get(), L"ADKオフラインレイアウト 新規識別");
    if (!observed || !observed.value().directory ||
        observed.value().reparse_point) {
      opened.value().reset();
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ADKオフラインレイアウト 新規識別",
          L"通常の非reparse新規directoryではありません");
    }
    layout_root_ = canonical.value();
    layout_manifest_id_ = std::string(manifest_id);
    layout_identity_ = observed.value().identity;
    layout_handle_ = opened.take_value();
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status publish_offline_layout_payload(
      const AdkPinnedPayload& payload,
      const AdkVerifiedPayload& verified_payload) {
    if (!layout_handle_ || layout_finalized_ ||
        verified_payload.kind != payload.kind ||
        verified_payload.installer_kind != payload.installer_kind ||
        verified_payload.byte_count != payload.expected_byte_count ||
        verified_payload.sha256 != payload.expected_sha256 ||
        !safe_leaf(payload.offline_relative_path) ||
        published_kinds_.contains(payload.kind)) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト publish契約",
          L"固定payload、basename、長さ、Hash、または一意kindが一致しません");
    }
    const auto root_status = verify_layout_root();
    if (!root_status) {
      return root_status;
    }
    clonecore::UniqueHandle source(CreateFileW(
        verified_payload.staged_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!source) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADKオフラインレイアウト source固定",
          GetLastError()));
    }
    const auto source_observation = observe_handle(
        source.get(), L"ADKオフラインレイアウト source識別");
    if (!source_observation ||
        !regular_single_non_reparse(source_observation.value()) ||
        source_observation.value().byte_count != payload.expected_byte_count) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト source識別",
          L"固定長の通常非reparse単一リンクsourceではありません");
    }
    const auto source_hash = hash_handle(
        source.get(),
        source_observation.value().byte_count,
        L"ADKオフラインレイアウト source SHA-256");
    if (!source_hash || digest_hex(source_hash.value()) !=
                            payload.expected_sha256) {
      return source_hash
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"ADKオフラインレイアウト source SHA-256",
                L"publish直前のsource Hashが固定値と一致しません")
          : clonecore::Status::failure(source_hash.error());
    }
    const auto destination_path =
        layout_root_ / payload.offline_relative_path;
    clonecore::UniqueHandle destination(CreateFileW(
        destination_path.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!destination) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADKオフラインレイアウト payload CREATE_NEW",
          GetLastError()));
    }
    const auto destination_observation = observe_handle(
        destination.get(), L"ADKオフラインレイアウト payload新規識別");
    if (!destination_observation ||
        !regular_single_non_reparse(destination_observation.value()) ||
        destination_observation.value().byte_count != 0U) {
      static_cast<void>(mark_delete(destination.get()));
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト payload新規識別",
          L"空の通常非reparse単一リンクCREATE_NEW fileではありません");
    }
    std::vector<std::byte> buffer(kIoBlockBytes);
    std::uint64_t copied{};
    while (copied < source_observation.value().byte_count) {
      const DWORD amount = static_cast<DWORD>(std::min<std::uint64_t>(
          source_observation.value().byte_count - copied,
          buffer.size()));
      DWORD read{};
      if (ReadFile(source.get(), buffer.data(), amount, &read, nullptr) ==
              FALSE ||
          read != amount) {
        static_cast<void>(mark_delete(destination.get()));
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"ADKオフラインレイアウト source完全読取り",
            GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF
                                            : GetLastError()));
      }
      DWORD written{};
      if (WriteFile(
              destination.get(), buffer.data(), amount, &written, nullptr) ==
              FALSE ||
          written != amount) {
        static_cast<void>(mark_delete(destination.get()));
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"ADKオフラインレイアウト payload完全書込み",
            GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT
                                            : GetLastError()));
      }
      copied += amount;
    }
    if (FlushFileBuffers(destination.get()) == FALSE) {
      const DWORD error = GetLastError();
      static_cast<void>(mark_delete(destination.get()));
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADKオフラインレイアウト payload flush",
          error));
    }
    const auto destination_hash = hash_handle(
        destination.get(),
        copied,
        L"ADKオフラインレイアウト payload読戻しSHA-256");
    if (!destination_hash || digest_hex(destination_hash.value()) !=
                                 payload.expected_sha256) {
      static_cast<void>(mark_delete(destination.get()));
      return destination_hash
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"ADKオフラインレイアウト payload読戻しSHA-256",
                L"保存後の完全読戻しHashが固定値と一致しません")
          : clonecore::Status::failure(destination_hash.error());
    }
    published_handles_.push_back(std::move(destination));
    published_kinds_.emplace(
        payload.kind, published_handles_.size() - 1U);
    published_files_.push_back(AdkOfflineLayoutPublishedFile{
        .kind = payload.kind,
        .file_name = payload.offline_relative_path.filename().native(),
        .byte_count = copied,
        .sha256 = payload.expected_sha256,
    });
    return verify_layout_root();
  }

  [[nodiscard]] clonecore::Result<AdkOfflineLayoutReport>
  finalize_offline_layout(const AdkReleaseManifest& manifest) {
    if (!layout_handle_ || layout_finalized_ ||
        layout_manifest_id_ != manifest.manifest_id ||
        published_files_.size() != manifest.payloads.size()) {
      return failure<AdkOfflineLayoutReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"ADKオフラインレイアウト 完了条件",
          L"同じmanifestの固定payloadをすべてpublishしていません");
    }
    for (const auto& payload : manifest.payloads) {
      if (!published_kinds_.contains(payload.kind)) {
        return failure<AdkOfflineLayoutReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"ADKオフラインレイアウト 完了kind検証",
            L"固定payload kindが欠落しています");
      }
    }
    const auto root_status = verify_layout_root();
    if (!root_status) {
      return clonecore::Result<AdkOfflineLayoutReport>::failure(
          root_status.error());
    }
    std::string manifest_text = "TSUMUGI_ADK_OFFLINE_LAYOUT_V1\nmanifest=";
    manifest_text.append(manifest.manifest_id).push_back('\n');
    for (const auto& file : published_files_) {
      const std::wstring wide_name(file.file_name);
      if (!std::all_of(
              wide_name.begin(), wide_name.end(), [](const wchar_t value) {
                return value >= 0x21 && value <= 0x7E;
              })) {
        return failure<AdkOfflineLayoutReport>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_NAME,
            L"ADKオフラインレイアウト manifest basename",
            L"固定ASCII basenameではありません");
      }
      std::string name;
      name.reserve(wide_name.size());
      for (const wchar_t value : wide_name) {
        name.push_back(static_cast<char>(value));
      }
      manifest_text.append("file=")
          .append(name)
          .append("|")
          .append(std::to_string(file.byte_count))
          .append("|")
          .append(file.sha256)
          .push_back('\n');
    }
    manifest_text.append("complete=1\n");
    std::vector<std::byte> manifest_bytes;
    manifest_bytes.reserve(manifest_text.size());
    for (const char value : manifest_text) {
      manifest_bytes.push_back(static_cast<std::byte>(
          static_cast<unsigned char>(value)));
    }
    const auto manifest_path =
        layout_root_ / std::wstring(kAdkOfflineLayoutManifestFileName);
    clonecore::UniqueHandle output(CreateFileW(
        manifest_path.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!output) {
      return clonecore::Result<AdkOfflineLayoutReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADKオフラインレイアウト complete manifest CREATE_NEW",
              GetLastError()));
    }
    const auto observed = observe_handle(
        output.get(), L"ADKオフラインレイアウト manifest新規識別");
    if (!observed || !regular_single_non_reparse(observed.value()) ||
        observed.value().byte_count != 0U) {
      static_cast<void>(mark_delete(output.get()));
      return failure<AdkOfflineLayoutReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト manifest新規識別",
          L"空の通常非reparse単一リンクmanifestではありません");
    }
    const auto written = write_all(
        output.get(),
        manifest_bytes,
        L"ADKオフラインレイアウト manifest書込み");
    if (!written || FlushFileBuffers(output.get()) == FALSE) {
      const auto error = written
          ? clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"ADKオフラインレイアウト manifest flush",
                GetLastError())
          : written.error();
      static_cast<void>(mark_delete(output.get()));
      return clonecore::Result<AdkOfflineLayoutReport>::failure(error);
    }
    published_handles_.push_back(std::move(output));
    layout_finalized_ = true;
    std::uint64_t total{};
    for (const auto& file : published_files_) {
      total += file.byte_count;
    }
    AdkOfflineLayoutReport report{
        .manifest_id = manifest.manifest_id,
        .layout_root = layout_root_,
        .total_bytes = total,
        .complete_manifest_written = true,
        .files = published_files_,
    };
    published_handles_.clear();
    layout_handle_.reset();
    layout_identity_.reset();
    layout_root_.clear();
    layout_manifest_id_.clear();
    published_files_.clear();
    published_kinds_.clear();
    layout_finalized_ = false;
    return clonecore::Result<AdkOfflineLayoutReport>::success(
        std::move(report));
  }

  [[nodiscard]] clonecore::Status abandon_offline_layout() {
    if (!layout_handle_ && layout_root_.empty()) {
      return clonecore::success_status();
    }
    clonecore::Error first_error{};
    bool failed{};
    for (auto iterator = published_handles_.rbegin();
         iterator != published_handles_.rend();
         ++iterator) {
      const auto removed = mark_delete(iterator->get());
      if (!removed && !failed) {
        first_error = removed.error();
        failed = true;
      }
    }
    published_handles_.clear();
    if (layout_handle_) {
      const auto removed = mark_delete(layout_handle_.get());
      if (!removed && !failed) {
        first_error = removed.error();
        failed = true;
      }
    }
    layout_handle_.reset();
    layout_root_.clear();
    layout_manifest_id_.clear();
    layout_identity_.reset();
    published_files_.clear();
    published_kinds_.clear();
    layout_finalized_ = false;
    return failed ? clonecore::Status::failure(std::move(first_error))
                  : clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<std::vector<std::uint32_t>>
  execute_managed_uninstall(const AdkUninstallPlan& plan) {
    if (!plan.requires_explicit_confirmation ||
        !plan.preserves_unmanaged_adk || plan.steps.empty()) {
      return failure<std::vector<std::uint32_t>>(
          clonecore::ErrorCode::confirmation_required,
          ERROR_CANCELLED,
          L"ADK管理対象削除 計画検証",
          L"明示確認と既存ADK保護を固定した削除計画ではありません");
    }
    std::vector<UninstallOperation> operations;
    operations.reserve(plan.steps.size());
    for (const auto& step : plan.steps) {
      const auto operation = parse_uninstall_registration(step);
      if (!operation) {
        return clonecore::Result<std::vector<std::uint32_t>>::failure(
            operation.error());
      }
      const std::wstring& product = operation.value().patch
          ? operation.value().product_guid
          : operation.value().first_guid;
      if (!msi_product_present(product)) {
        return failure<std::vector<std::uint32_t>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_UNKNOWN_PRODUCT,
            L"ADK管理対象削除 MSI登録再確認",
            L"固定product GUIDをWindows Installer登録から再確認できません");
      }
      operations.push_back(operation.value());
    }
    std::vector<std::uint32_t> exit_codes;
    exit_codes.reserve(operations.size());
    for (const auto& operation : operations) {
      const UINT status = operation.patch
          ? MsiRemovePatchesW(
                operation.first_guid.c_str(),
                operation.product_guid.c_str(),
                INSTALLTYPE_SINGLE_INSTANCE,
                L"REBOOT=ReallySuppress")
          : MsiConfigureProductExW(
                operation.first_guid.c_str(),
                INSTALLLEVEL_DEFAULT,
                INSTALLSTATE_ABSENT,
                L"REBOOT=ReallySuppress");
      exit_codes.push_back(status);
      if (status != ERROR_SUCCESS && status != ERROR_SUCCESS_REBOOT_REQUIRED) {
        return failure<std::vector<std::uint32_t>>(
            clonecore::ErrorCode::io_failed,
            status,
            L"ADK管理対象 Windows Installer削除",
            L"固定登録識別子の管理対象削除に失敗しました");
      }
    }
    return clonecore::Result<std::vector<std::uint32_t>>::success(
        std::move(exit_codes));
  }

 private:
  [[nodiscard]] clonecore::Status verify_layout_root() {
    if (!layout_handle_ || !layout_identity_.has_value()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_HANDLE,
          L"ADKオフラインレイアウト root再識別",
          L"保持中の新規root handleがありません");
    }
    const auto held = observe_handle(
        layout_handle_.get(), L"ADKオフラインレイアウト held root識別");
    if (!held || !held.value().directory || held.value().reparse_point ||
        held.value().identity != layout_identity_.value()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト held root識別",
          L"作成時の通常非reparse rootと一致しません");
    }
    const auto reopened = open_normal_directory(
        layout_root_,
        L"ADKオフラインレイアウト path root再識別",
        FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (!reopened) {
      return clonecore::Status::failure(reopened.error());
    }
    const auto current = observe_handle(
        reopened.value().get(),
        L"ADKオフラインレイアウト path root現在識別");
    if (!current || current.value().identity != layout_identity_.value()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト path root現在識別",
          L"現在pathが作成時のroot identityと一致しません");
    }
    return clonecore::success_status();
  }

  std::filesystem::path layout_root_;
  std::string layout_manifest_id_;
  std::optional<FileIdentity> layout_identity_;
  clonecore::UniqueHandle layout_handle_;
  std::vector<clonecore::UniqueHandle> published_handles_;
  std::vector<AdkOfflineLayoutPublishedFile> published_files_;
  std::map<AdkPayloadKind, std::size_t> published_kinds_;
  bool layout_finalized_{};
};

WindowsAdkManagementPlatform::WindowsAdkManagementPlatform()
    : impl_(std::make_unique<Impl>()) {}

WindowsAdkManagementPlatform::~WindowsAdkManagementPlatform() = default;

clonecore::Result<std::optional<AdkManagedInstallationRecord>>
WindowsAdkManagementPlatform::load_managed_installation_record() {
  return impl_->load_managed_installation_record();
}

clonecore::Status
WindowsAdkManagementPlatform::save_managed_installation_record_create_new(
    const AdkManagedInstallationRecord& record) {
  return impl_->save_managed_installation_record_create_new(record);
}

clonecore::Status
WindowsAdkManagementPlatform::remove_managed_installation_record_if_exact(
    const AdkManagedInstallationRecord& record) {
  return impl_->remove_managed_installation_record_if_exact(record);
}

clonecore::Status WindowsAdkManagementPlatform::begin_new_offline_layout(
    const std::filesystem::path& layout_root,
    const std::string_view manifest_id) {
  return impl_->begin_new_offline_layout(layout_root, manifest_id);
}

clonecore::Status
WindowsAdkManagementPlatform::publish_offline_layout_payload(
    const AdkPinnedPayload& payload,
    const AdkVerifiedPayload& verified_payload) {
  return impl_->publish_offline_layout_payload(payload, verified_payload);
}

clonecore::Result<AdkOfflineLayoutReport>
WindowsAdkManagementPlatform::finalize_offline_layout(
    const AdkReleaseManifest& manifest) {
  return impl_->finalize_offline_layout(manifest);
}

clonecore::Status WindowsAdkManagementPlatform::abandon_offline_layout() {
  return impl_->abandon_offline_layout();
}

clonecore::Result<std::vector<std::uint32_t>>
WindowsAdkManagementPlatform::execute_managed_uninstall(
    const AdkUninstallPlan& plan) {
  return impl_->execute_managed_uninstall(plan);
}

std::unique_ptr<IAdkManagementPlatform>
make_windows_adk_management_platform() {
  return std::make_unique<WindowsAdkManagementPlatform>();
}

}  // namespace ytec::windowsapp

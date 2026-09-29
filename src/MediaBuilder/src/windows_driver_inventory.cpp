#include "ytec/mediabuilder/driver_selection.h"

#include "ytec/clonecore/error.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <bcrypt.h>
#include <SetupAPI.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::mediabuilder {
namespace {

constexpr std::uint64_t kMaximumPackageBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumPackageFileBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumPackageFiles = 4096U;
constexpr DWORD kMaximumPathCharacters = 32768U;
constexpr std::size_t kMaximumClassNameCharacters = 256U;
constexpr DWORD kHashReadBytes = 1024U * 1024U;

constexpr GUID kStorageAdapterClass{
    0x4d36e97b,
    0xe325,
    0x11ce,
    {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr GUID kIdeControllerClass{
    0x4d36e96a,
    0xe325,
    0x11ce,
    {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr GUID kUsbControllerClass{
    0x36fc9e60,
    0xc465,
    0x11cf,
    {0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

constexpr DEVPROPKEY kDriverInfPathProperty{
    {0xa8b865dd,
     0x2e3d,
     0x4094,
     {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}},
    5U};
constexpr DEVPROPKEY kDriverProviderProperty{
    {0xa8b865dd,
     0x2e3d,
     0x4094,
     {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6}},
    9U};

struct PackageSnapshot final {
  std::filesystem::path canonical_root;
  std::string tree_sha256;
  std::uint64_t total_bytes{};
  std::size_t file_count{};
};

struct FileSnapshot final {
  std::filesystem::path canonical_path;
  std::string sha256;
  std::uint64_t bytes{};
};

struct PackageEntry final {
  std::filesystem::path path;
  std::wstring relative_lower;
  std::uint64_t bytes{};
  std::array<std::byte, 32U> digest{};
};

clonecore::Error driver_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring_view operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::wstring(operation),
      .message = std::move(message),
  };
}

DriverDiagnostic diagnostic_from_error(
    const std::string& code,
    const std::filesystem::path& path,
    const clonecore::Error& error) {
  return DriverDiagnostic{
      .severity = DiagnosticSeverity::error,
      .code = code,
      .path = path,
      .message = error.message,
      .native_code = error.native_code,
  };
}

void append_diagnostic(
    DriverDiscoveryReport& report,
    std::string code,
    const std::filesystem::path& path,
    std::wstring message,
    const DWORD native_code = ERROR_SUCCESS,
    const DiagnosticSeverity severity = DiagnosticSeverity::error) {
  report.diagnostics.push_back(DriverDiagnostic{
      .severity = severity,
      .code = std::move(code),
      .path = path,
      .message = std::move(message),
      .native_code = native_code,
  });
}

class AlgorithmHandle final {
 public:
  ~AlgorithmHandle() noexcept {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0U);
    }
  }
  AlgorithmHandle() = default;
  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class HashHandle final {
 public:
  ~HashHandle() noexcept {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }
  HashHandle() = default;
  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;

  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

class Sha256 final {
 public:
  [[nodiscard]] clonecore::Status initialize() {
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        algorithm_.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Status::failure(driver_error(
          clonecore::ErrorCode::verification_failed,
          static_cast<DWORD>(status),
          L"ドライバーパッケージSHA-256初期化",
          L"Windows CNG SHA-256を初期化できません"));
    }

    ULONG returned{};
    status = BCryptGetProperty(
        algorithm_.get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_bytes_),
        sizeof(object_bytes_),
        &returned,
        0U);
    if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_bytes_) ||
        object_bytes_ == 0U) {
      return clonecore::Status::failure(driver_error(
          clonecore::ErrorCode::verification_failed,
          static_cast<DWORD>(status),
          L"ドライバーパッケージSHA-256オブジェクト長",
          L"SHA-256オブジェクト長を確認できません"));
    }

    ULONG digest_bytes{};
    status = BCryptGetProperty(
        algorithm_.get(),
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&digest_bytes),
        sizeof(digest_bytes),
        &returned,
        0U);
    if (!BCRYPT_SUCCESS(status) || returned != sizeof(digest_bytes) ||
        digest_bytes != 32U) {
      return clonecore::Status::failure(driver_error(
          clonecore::ErrorCode::verification_failed,
          static_cast<DWORD>(status),
          L"ドライバーパッケージSHA-256出力長",
          L"SHA-256出力長が期待値と一致しません"));
    }

    object_.resize(object_bytes_);
    status = BCryptCreateHash(
        algorithm_.get(),
        hash_.put(),
        object_.data(),
        object_bytes_,
        nullptr,
        0U,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Status::failure(driver_error(
          clonecore::ErrorCode::verification_failed,
          static_cast<DWORD>(status),
          L"ドライバーパッケージSHA-256生成",
          L"SHA-256ハッシュを生成できません"));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status update(const std::span<const std::byte> data) {
    std::size_t offset{};
    while (offset < data.size()) {
      const ULONG amount = static_cast<ULONG>(std::min<std::size_t>(
          data.size() - offset,
          std::numeric_limits<ULONG>::max()));
      const NTSTATUS status = BCryptHashData(
          hash_.get(),
          reinterpret_cast<PUCHAR>(
              const_cast<std::byte*>(data.data() + offset)),
          amount,
          0U);
      if (!BCRYPT_SUCCESS(status)) {
        return clonecore::Status::failure(driver_error(
            clonecore::ErrorCode::verification_failed,
            static_cast<DWORD>(status),
            L"ドライバーパッケージSHA-256更新",
            L"SHA-256入力を処理できません"));
      }
      offset += amount;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<std::array<std::byte, 32U>> finish() {
    std::array<std::byte, 32U> digest{};
    const NTSTATUS status = BCryptFinishHash(
        hash_.get(),
        reinterpret_cast<PUCHAR>(digest.data()),
        static_cast<ULONG>(digest.size()),
        0U);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Result<std::array<std::byte, 32U>>::failure(
          driver_error(
              clonecore::ErrorCode::verification_failed,
              static_cast<DWORD>(status),
              L"ドライバーパッケージSHA-256完了",
              L"SHA-256を確定できません"));
    }
    return clonecore::Result<std::array<std::byte, 32U>>::success(digest);
  }

 private:
  // BCryptDestroyHash can still access the caller-owned object buffer.
  // Reverse destruction order must therefore be hash -> object -> algorithm.
  AlgorithmHandle algorithm_;
  ULONG object_bytes_{};
  std::vector<UCHAR> object_;
  HashHandle hash_;
};

std::string digest_hex(const std::array<std::byte, 32U>& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.resize(digest.size() * 2U);
  for (std::size_t index = 0; index < digest.size(); ++index) {
    const auto value = std::to_integer<unsigned int>(digest[index]);
    text[index * 2U] = kHex[(value >> 4U) & 0x0FU];
    text[index * 2U + 1U] = kHex[value & 0x0FU];
  }
  return text;
}

std::wstring lower_text(std::wstring value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
      });
  return value;
}

bool equal_text_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  if (left.size() > static_cast<std::size_t>(INT_MAX) ||
      right.size() > static_cast<std::size_t>(INT_MAX)) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

std::filesystem::path strip_extended_prefix(std::filesystem::path path) {
  std::wstring value = path.native();
  constexpr std::wstring_view kExtendedUnc = L"\\\\?\\UNC\\";
  constexpr std::wstring_view kExtended = L"\\\\?\\";
  if (value.starts_with(kExtendedUnc)) {
    value = L"\\\\" + value.substr(kExtendedUnc.size());
  } else if (value.starts_with(kExtended)) {
    value.erase(0U, kExtended.size());
  }
  return std::filesystem::path(std::move(value)).lexically_normal();
}

clonecore::Result<std::filesystem::path> final_path_for_handle(
    const HANDLE handle,
    const std::wstring_view operation) {
  DWORD required = GetFinalPathNameByHandleW(
      handle, nullptr, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (required == 0U || required > kMaximumPathCharacters) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        operation,
        L"最終パスの長さを安全に確認できません"));
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
  const DWORD written = GetFinalPathNameByHandleW(
      handle,
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0U || written >= buffer.size()) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        operation,
        L"最終パスを取得できません"));
  }
  return clonecore::Result<std::filesystem::path>::success(
      strip_extended_prefix(std::filesystem::path(
          std::wstring(buffer.data(), written))));
}

bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& child) {
  std::wstring root_text = lower_text(root.lexically_normal().native());
  const std::wstring child_text = lower_text(child.lexically_normal().native());
  while (root_text.size() > 3U &&
         (root_text.back() == L'\\' || root_text.back() == L'/')) {
    root_text.pop_back();
  }
  if (child_text == root_text) {
    return true;
  }
  return child_text.size() > root_text.size() &&
         child_text.compare(0U, root_text.size(), root_text) == 0 &&
         (child_text[root_text.size()] == L'\\' ||
          child_text[root_text.size()] == L'/');
}

clonecore::Result<std::filesystem::path> inspect_directory(
    const std::filesystem::path& path,
    const std::optional<std::filesystem::path>& required_parent = std::nullopt) {
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      0U,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle.valid()) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"ドライバーフォルダー確認",
        L"フォルダーを読取り専用で開けません"));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          handle.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"ドライバーフォルダー属性確認",
        L"フォルダー属性を確認できません"));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        L"ドライバーフォルダー属性確認",
        L"通常フォルダーではないか、リンク／再解析ポイントです"));
  }
  auto final_path = final_path_for_handle(
      handle.get(), L"ドライバーフォルダー最終パス確認");
  if (!final_path) {
    return final_path;
  }
  if (required_parent.has_value() &&
      !path_is_within(*required_parent, final_path.value())) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_BAD_PATHNAME,
        L"ドライバーフォルダー範囲確認",
        L"フォルダーの最終パスが承認されたルート外へ移動しました"));
  }
  return final_path;
}

clonecore::Result<FileSnapshot> inspect_and_hash_file(
    const std::filesystem::path& path,
    const std::optional<std::filesystem::path>& required_parent = std::nullopt) {
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!handle.valid()) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::io_failed,
        GetLastError(),
        L"ドライバーファイル読取り",
        L"ドライバーファイルを読取り専用で開けません"));
  }

  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          handle.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"ドライバーファイル属性確認",
        L"ドライバーファイル属性を確認できません"));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE |
        FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        L"ドライバーファイル属性確認",
        L"通常ファイルではないか、リンク／再解析ポイントです"));
  }

  auto final_path = final_path_for_handle(
      handle.get(), L"ドライバーファイル最終パス確認");
  if (!final_path) {
    return clonecore::Result<FileSnapshot>::failure(final_path.error());
  }
  if (required_parent.has_value() &&
      !path_is_within(*required_parent, final_path.value())) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_BAD_PATHNAME,
        L"ドライバーファイル範囲確認",
        L"ファイルの最終パスが承認されたルート外へ移動しました"));
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"ドライバーファイル寸法確認",
        L"ファイル寸法を確認できません"));
  }
  const auto bytes = static_cast<std::uint64_t>(size.QuadPart);
  if (bytes > kMaximumPackageFileBytes) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"ドライバーファイル寸法確認",
        L"単一ファイルが安全上限を超えています"));
  }

  Sha256 hasher;
  const auto initialized = hasher.initialize();
  if (!initialized) {
    return clonecore::Result<FileSnapshot>::failure(initialized.error());
  }
  std::vector<std::byte> buffer(kHashReadBytes);
  std::uint64_t consumed{};
  while (consumed < bytes) {
    const DWORD requested = static_cast<DWORD>(std::min<std::uint64_t>(
        bytes - consumed, buffer.size()));
    DWORD read{};
    if (!ReadFile(handle.get(), buffer.data(), requested, &read, nullptr)) {
      return clonecore::Result<FileSnapshot>::failure(driver_error(
          clonecore::ErrorCode::io_failed,
          GetLastError(),
          L"ドライバーファイルSHA-256読取り",
          L"ファイルを完全に読み取れません"));
    }
    if (read != requested) {
      return clonecore::Result<FileSnapshot>::failure(driver_error(
          clonecore::ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"ドライバーファイルSHA-256読取り",
          L"ファイルが読取り中に短くなりました"));
    }
    const auto updated = hasher.update(std::span<const std::byte>(
        buffer.data(), static_cast<std::size_t>(read)));
    if (!updated) {
      return clonecore::Result<FileSnapshot>::failure(updated.error());
    }
    consumed += read;
  }
  auto digest = hasher.finish();
  if (!digest) {
    return clonecore::Result<FileSnapshot>::failure(digest.error());
  }

  LARGE_INTEGER after_size{};
  if (!GetFileSizeEx(handle.get(), &after_size) ||
      after_size.QuadPart != size.QuadPart) {
    return clonecore::Result<FileSnapshot>::failure(driver_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"ドライバーファイル読取り後確認",
        L"ファイルが読取り中に差し替えられました"));
  }

  return clonecore::Result<FileSnapshot>::success(FileSnapshot{
      .canonical_path = final_path.take_value(),
      .sha256 = digest_hex(digest.value()),
      .bytes = bytes,
  });
}

clonecore::Result<PackageSnapshot> snapshot_package_tree(
    const std::filesystem::path& requested_root) {
  auto inspected_root = inspect_directory(requested_root);
  if (!inspected_root) {
    return clonecore::Result<PackageSnapshot>::failure(inspected_root.error());
  }
  const std::filesystem::path root = inspected_root.take_value();

  std::vector<PackageEntry> entries;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root,
      std::filesystem::directory_options::none,
      error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    return clonecore::Result<PackageSnapshot>::failure(driver_error(
        clonecore::ErrorCode::enumeration_failed,
        static_cast<DWORD>(error.value()),
        L"ドライバーパッケージ列挙",
        L"パッケージフォルダーを列挙できません"));
  }

  std::uint64_t total_bytes{};
  while (iterator != end) {
    const std::filesystem::path entry_path = iterator->path();
    const auto status = iterator->symlink_status(error);
    if (error) {
      return clonecore::Result<PackageSnapshot>::failure(driver_error(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(error.value()),
          L"ドライバーパッケージ属性列挙",
          L"パッケージ要素の属性を確認できません"));
    }
    if (std::filesystem::is_symlink(status) ||
        std::filesystem::is_other(status)) {
      return clonecore::Result<PackageSnapshot>::failure(driver_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ドライバーパッケージリンク確認",
          L"パッケージ内にリンクまたは通常外の要素があります"));
    }
    if (std::filesystem::is_directory(status)) {
      auto directory = inspect_directory(entry_path, root);
      if (!directory) {
        return clonecore::Result<PackageSnapshot>::failure(directory.error());
      }
    } else if (std::filesystem::is_regular_file(status)) {
      auto file = inspect_and_hash_file(entry_path, root);
      if (!file) {
        return clonecore::Result<PackageSnapshot>::failure(file.error());
      }
      if (entries.size() >= kMaximumPackageFiles ||
          file.value().bytes > kMaximumPackageBytes - total_bytes) {
        return clonecore::Result<PackageSnapshot>::failure(driver_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_FILE_TOO_LARGE,
            L"ドライバーパッケージ安全上限",
            L"パッケージのファイル数または合計寸法が安全上限を超えています"));
      }
      const auto relative = file.value().canonical_path.lexically_relative(root);
      if (relative.empty() || relative.is_absolute()) {
        return clonecore::Result<PackageSnapshot>::failure(driver_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_BAD_PATHNAME,
            L"ドライバーパッケージ相対パス",
            L"パッケージ内相対パスを確定できません"));
      }
      std::array<std::byte, 32U> digest{};
      for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto hex_value = [](const char character) -> unsigned int {
          return character <= '9'
                     ? static_cast<unsigned int>(character - '0')
                     : static_cast<unsigned int>(character - 'a' + 10);
        };
        digest[index] = static_cast<std::byte>(
            (hex_value(file.value().sha256[index * 2U]) << 4U) |
            hex_value(file.value().sha256[index * 2U + 1U]));
      }
      total_bytes += file.value().bytes;
      entries.push_back(PackageEntry{
          .path = file.value().canonical_path,
          .relative_lower = lower_text(relative.generic_wstring()),
          .bytes = file.value().bytes,
          .digest = digest,
      });
    } else {
      return clonecore::Result<PackageSnapshot>::failure(driver_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ドライバーパッケージ要素確認",
          L"パッケージ内に通常ファイル／フォルダー以外の要素があります"));
    }
    iterator.increment(error);
    if (error) {
      return clonecore::Result<PackageSnapshot>::failure(driver_error(
          clonecore::ErrorCode::enumeration_failed,
          static_cast<DWORD>(error.value()),
          L"ドライバーパッケージ列挙継続",
          L"パッケージの再帰列挙を完了できません"));
    }
  }
  if (entries.empty()) {
    return clonecore::Result<PackageSnapshot>::failure(driver_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_NOT_FOUND,
        L"ドライバーパッケージ内容確認",
        L"パッケージに通常ファイルがありません"));
  }

  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return left.relative_lower < right.relative_lower;
  });
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index - 1U].relative_lower == entries[index].relative_lower) {
      return clonecore::Result<PackageSnapshot>::failure(driver_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"ドライバーパッケージ名前確認",
          L"大文字小文字だけが異なる重複パスがあります"));
    }
  }

  Sha256 tree_hash;
  const auto initialized = tree_hash.initialize();
  if (!initialized) {
    return clonecore::Result<PackageSnapshot>::failure(initialized.error());
  }
  for (const auto& entry : entries) {
    const auto path_bytes = std::as_bytes(std::span<const wchar_t>(
        entry.relative_lower.data(), entry.relative_lower.size()));
    auto updated = tree_hash.update(path_bytes);
    if (!updated) {
      return clonecore::Result<PackageSnapshot>::failure(updated.error());
    }
    const std::array<std::byte, sizeof(std::uint64_t)> size_bytes = {
        static_cast<std::byte>(entry.bytes & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 8U) & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 16U) & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 24U) & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 32U) & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 40U) & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 48U) & 0xFFU),
        static_cast<std::byte>((entry.bytes >> 56U) & 0xFFU),
    };
    updated = tree_hash.update(size_bytes);
    if (!updated) {
      return clonecore::Result<PackageSnapshot>::failure(updated.error());
    }
    updated = tree_hash.update(entry.digest);
    if (!updated) {
      return clonecore::Result<PackageSnapshot>::failure(updated.error());
    }
  }
  auto digest = tree_hash.finish();
  if (!digest) {
    return clonecore::Result<PackageSnapshot>::failure(digest.error());
  }
  return clonecore::Result<PackageSnapshot>::success(PackageSnapshot{
      .canonical_root = root,
      .tree_sha256 = digest_hex(digest.value()),
      .total_bytes = total_bytes,
      .file_count = entries.size(),
  });
}

std::optional<DriverCategory> category_for_guid(const GUID& guid) noexcept {
  if (IsEqualGUID(guid, kStorageAdapterClass) ||
      IsEqualGUID(guid, kIdeControllerClass)) {
    return DriverCategory::storage_controller;
  }
  if (IsEqualGUID(guid, kUsbControllerClass)) {
    return DriverCategory::usb_controller;
  }
  return std::nullopt;
}

SP_ALTPLATFORM_INFO make_amd64_platform() noexcept {
  SP_ALTPLATFORM_INFO platform{};
  platform.cbSize = sizeof(platform);
  platform.Platform = VER_PLATFORM_WIN32_NT;
  platform.MajorVersion = 10U;
  platform.MinorVersion = 0U;
  platform.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
  platform.Flags = SP_ALTPLATFORM_FLAGS_VERSION_RANGE;
  platform.FirstValidatedMajorVersion = 10U;
  platform.FirstValidatedMinorVersion = 0U;
  return platform;
}

std::wstring query_device_property_string(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device,
    const DEVPROPKEY& key) {
  DEVPROPTYPE type{};
  DWORD required{};
  SetupDiGetDevicePropertyW(
      devices, &device, &key, &type, nullptr, 0U, &required, 0U);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      type != DEVPROP_TYPE_STRING || required < sizeof(wchar_t) ||
      required > kMaximumPathCharacters * sizeof(wchar_t) ||
      required % sizeof(wchar_t) != 0U) {
    return {};
  }
  std::vector<std::byte> buffer(required);
  if (!SetupDiGetDevicePropertyW(
          devices,
          &device,
          &key,
          &type,
          reinterpret_cast<PBYTE>(buffer.data()),
          static_cast<DWORD>(buffer.size()),
          nullptr,
          0U) ||
      type != DEVPROP_TYPE_STRING) {
    return {};
  }
  const auto* text = reinterpret_cast<const wchar_t*>(buffer.data());
  const std::size_t characters = buffer.size() / sizeof(wchar_t);
  const auto terminator = std::find(text, text + characters, L'\0');
  if (terminator == text + characters) {
    return {};
  }
  return std::wstring(text, terminator);
}

std::wstring query_device_registry_string(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device,
    const DWORD property) {
  DWORD type{};
  DWORD required{};
  SetupDiGetDeviceRegistryPropertyW(
      devices, &device, property, &type, nullptr, 0U, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || type != REG_SZ ||
      required < sizeof(wchar_t) ||
      required > kMaximumPathCharacters * sizeof(wchar_t) ||
      required % sizeof(wchar_t) != 0U) {
    return {};
  }
  std::vector<std::byte> buffer(required);
  if (!SetupDiGetDeviceRegistryPropertyW(
          devices,
          &device,
          property,
          &type,
          reinterpret_cast<PBYTE>(buffer.data()),
          static_cast<DWORD>(buffer.size()),
          nullptr) ||
      type != REG_SZ) {
    return {};
  }
  const auto* text = reinterpret_cast<const wchar_t*>(buffer.data());
  const std::size_t characters = buffer.size() / sizeof(wchar_t);
  const auto terminator = std::find(text, text + characters, L'\0');
  if (terminator == text + characters) {
    return {};
  }
  return std::wstring(text, terminator);
}

std::optional<std::filesystem::path> windows_directory() {
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  UINT written = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (written == 0U) {
    return std::nullopt;
  }
  if (written >= buffer.size()) {
    if (written > kMaximumPathCharacters) {
      return std::nullopt;
    }
    buffer.assign(static_cast<std::size_t>(written) + 1U, L'\0');
    written = GetWindowsDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (written == 0U || written >= buffer.size()) {
      return std::nullopt;
    }
  }
  return std::filesystem::path(std::wstring(buffer.data(), written));
}

clonecore::Result<std::filesystem::path> resolve_driver_store_inf(
    const std::wstring_view published_name) {
  if (published_name.empty() || published_name.size() > MAX_PATH ||
      published_name.find_first_of(L"\\/") != std::wstring_view::npos ||
      !equal_text_case_insensitive(
          std::filesystem::path(published_name).extension().native(), L".inf")) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"現在PCドライバーINF名確認",
        L"PnPが返したINF公開名が単純な.infファイル名ではありません"));
  }
  const auto windows = windows_directory();
  if (!windows.has_value()) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"Windowsフォルダー確認",
        L"Windowsフォルダーを確認できません"));
  }
  const std::filesystem::path published_path =
      *windows / L"INF" / std::filesystem::path(published_name);
  SP_ALTPLATFORM_INFO platform = make_amd64_platform();
  DWORD required{};
  SetupGetInfDriverStoreLocationW(
      published_path.c_str(), &platform, nullptr, nullptr, 0U, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0U ||
      required > kMaximumPathCharacters) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"DriverStore INFパス長確認",
        L"使用中ドライバーのDriverStore位置を確認できません"));
  }
  std::vector<wchar_t> buffer(required, L'\0');
  if (!SetupGetInfDriverStoreLocationW(
          published_path.c_str(),
          &platform,
          nullptr,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          nullptr)) {
    return clonecore::Result<std::filesystem::path>::failure(driver_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"DriverStore INFパス確認",
        L"使用中ドライバーのDriverStore位置を取得できません"));
  }
  return clonecore::Result<std::filesystem::path>::success(
      std::filesystem::path(buffer.data()));
}

DriverArchitectureState query_inf_amd64_architecture(
    const std::filesystem::path& inf_path) {
  UINT error_line{};
  HINF inf = SetupOpenInfFileW(
      inf_path.c_str(), nullptr, INF_STYLE_WIN4, &error_line);
  if (inf == INVALID_HANDLE_VALUE) {
    return DriverArchitectureState::unknown;
  }
  struct InfCloser final {
    HINF handle{INVALID_HANDLE_VALUE};
    ~InfCloser() noexcept {
      if (handle != INVALID_HANDLE_VALUE) {
        SetupCloseInfFile(handle);
      }
    }
  } closer{inf};

  INFCONTEXT context{};
  if (!SetupFindFirstLineW(inf, L"Manufacturer", nullptr, &context)) {
    return DriverArchitectureState::unknown;
  }
  bool saw_other_architecture{};
  for (;;) {
    const DWORD fields = SetupGetFieldCount(&context);
    for (DWORD field = 2U; field <= fields; ++field) {
      DWORD required{};
      SetupGetStringFieldW(&context, field, nullptr, 0U, &required);
      if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0U ||
          required > kMaximumPathCharacters) {
        continue;
      }
      std::vector<wchar_t> buffer(required, L'\0');
      if (!SetupGetStringFieldW(
              &context,
              field,
              buffer.data(),
              static_cast<DWORD>(buffer.size()),
              nullptr)) {
        continue;
      }
      const std::wstring decoration = lower_text(buffer.data());
      if (decoration == L"ntamd64" || decoration.starts_with(L"ntamd64.")) {
        return DriverArchitectureState::amd64_verified;
      }
      if (decoration == L"ntx86" || decoration.starts_with(L"ntx86.") ||
          decoration == L"ntarm64" || decoration.starts_with(L"ntarm64.") ||
          decoration == L"ntia64" || decoration.starts_with(L"ntia64.")) {
        saw_other_architecture = true;
      }
    }
    INFCONTEXT next{};
    if (!SetupFindNextLine(&context, &next)) {
      break;
    }
    context = next;
  }
  return saw_other_architecture ? DriverArchitectureState::incompatible
                                : DriverArchitectureState::unknown;
}

struct SignatureEvidence final {
  DriverSignatureState state{DriverSignatureState::unknown};
  std::filesystem::path catalog_path;
  std::wstring signer;
  DWORD native_code{};
};

SignatureEvidence verify_inf_signature(
    const std::filesystem::path& inf_path) {
  SP_ALTPLATFORM_INFO platform = make_amd64_platform();
  SP_INF_SIGNER_INFO signer{};
  signer.cbSize = sizeof(signer);
  if (!SetupVerifyInfFileW(inf_path.c_str(), &platform, &signer)) {
    const DWORD native_code = GetLastError();
    const bool verification_answer =
        native_code == ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED ||
        native_code == static_cast<DWORD>(TRUST_E_NOSIGNATURE) ||
        native_code == static_cast<DWORD>(TRUST_E_BAD_DIGEST) ||
        native_code == static_cast<DWORD>(CERT_E_UNTRUSTEDROOT) ||
        native_code == static_cast<DWORD>(CERT_E_CHAINING);
    return SignatureEvidence{
        .state = verification_answer
                     ? DriverSignatureState::unsigned_or_untrusted
                     : DriverSignatureState::unknown,
        .native_code = native_code,
    };
  }
  if (signer.CatalogFile[0] == L'\0' || signer.DigitalSigner[0] == L'\0') {
    return SignatureEvidence{
        .state = DriverSignatureState::unknown,
        .native_code = ERROR_INVALID_DATA,
    };
  }
  std::filesystem::path catalog(signer.CatalogFile);
  if (!catalog.is_absolute()) {
    catalog = inf_path.parent_path() / catalog;
  }
  auto catalog_file = inspect_and_hash_file(catalog);
  if (!catalog_file) {
    return SignatureEvidence{
        .state = DriverSignatureState::unknown,
        .native_code = catalog_file.error().native_code,
    };
  }
  return SignatureEvidence{
      .state = DriverSignatureState::trusted_signed,
      .catalog_path = catalog_file.value().canonical_path,
      .signer = signer.DigitalSigner,
      .native_code = ERROR_SUCCESS,
  };
}

clonecore::Result<std::string> candidate_id(
    const DriverOrigin origin,
    const DriverCategory category,
    const std::filesystem::path& inf_path) {
  Sha256 hash;
  const auto initialized = hash.initialize();
  if (!initialized) {
    return clonecore::Result<std::string>::failure(initialized.error());
  }
  const std::array<std::byte, 2U> prefix{
      static_cast<std::byte>(origin), static_cast<std::byte>(category)};
  auto updated = hash.update(prefix);
  if (!updated) {
    return clonecore::Result<std::string>::failure(updated.error());
  }
  const std::wstring lower_path = lower_text(inf_path.lexically_normal().native());
  updated = hash.update(std::as_bytes(std::span<const wchar_t>(
      lower_path.data(), lower_path.size())));
  if (!updated) {
    return clonecore::Result<std::string>::failure(updated.error());
  }
  auto digest = hash.finish();
  if (!digest) {
    return clonecore::Result<std::string>::failure(digest.error());
  }
  return clonecore::Result<std::string>::success(digest_hex(digest.value()));
}

clonecore::Result<DriverCandidateEvidence> inspect_inf_candidate(
    const DriverOrigin origin,
    const DriverCategory category,
    std::wstring display_name,
    std::wstring provider,
    std::vector<std::wstring> device_names,
    const std::filesystem::path& requested_inf,
    const DriverArchitectureState architecture,
    const std::optional<std::filesystem::path>& approved_root) {
  const std::filesystem::path requested_root = requested_inf.parent_path();
  auto package = snapshot_package_tree(requested_root);
  if (!package) {
    return clonecore::Result<DriverCandidateEvidence>::failure(package.error());
  }
  if (approved_root.has_value() &&
      !path_is_within(*approved_root, package.value().canonical_root)) {
    return clonecore::Result<DriverCandidateEvidence>::failure(driver_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_BAD_PATHNAME,
        L"ドライバーパッケージ承認範囲確認",
        L"パッケージの最終パスが承認されたルート外へ移動しました"));
  }
  auto inf = inspect_and_hash_file(requested_inf, package.value().canonical_root);
  if (!inf) {
    return clonecore::Result<DriverCandidateEvidence>::failure(inf.error());
  }
  if (!equal_text_case_insensitive(inf.value().canonical_path.extension().native(), L".inf")) {
    return clonecore::Result<DriverCandidateEvidence>::failure(driver_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"ドライバーINF拡張子確認",
        L"選択対象が.inf通常ファイルではありません"));
  }
  const SignatureEvidence signature = verify_inf_signature(inf.value().canonical_path);
  auto id = candidate_id(origin, category, inf.value().canonical_path);
  if (!id) {
    return clonecore::Result<DriverCandidateEvidence>::failure(id.error());
  }

  std::vector<DriverDiagnostic> diagnostics;
  if (signature.state != DriverSignatureState::trusted_signed) {
    diagnostics.push_back(DriverDiagnostic{
        .severity = DiagnosticSeverity::error,
        .code = "DRIVER_SIGNATURE_NOT_TRUSTED",
        .path = inf.value().canonical_path,
        .message = L"INFカタログ署名とローカル信頼チェーンを確認できません",
        .native_code = signature.native_code,
    });
  }
  if (architecture != DriverArchitectureState::amd64_verified) {
    diagnostics.push_back(DriverDiagnostic{
        .severity = DiagnosticSeverity::error,
        .code = "DRIVER_AMD64_NOT_VERIFIED",
        .path = inf.value().canonical_path,
        .message = L"INFのx64（amd64）互換性を確認できません",
    });
  }

  return clonecore::Result<DriverCandidateEvidence>::success(
      DriverCandidateEvidence{
          .candidate_id = id.take_value(),
          .origin = origin,
          .category = category,
          .display_name = std::move(display_name),
          .provider = std::move(provider),
          .present_device_names = std::move(device_names),
          .inf_path = inf.value().canonical_path,
          .package_root = package.value().canonical_root,
          .catalog_path = signature.catalog_path,
          .architecture = architecture,
          .signature = signature.state,
          .path_state = DriverPathState::verified_regular_tree,
          .signer = signature.signer,
          .inf_sha256 = inf.value().sha256,
          .package_tree_sha256 = package.value().tree_sha256,
          .package_total_bytes = package.value().total_bytes,
          .package_file_count = package.value().file_count,
          .diagnostics = std::move(diagnostics),
      });
}

class DeviceInfoSet final {
 public:
  explicit DeviceInfoSet(const HDEVINFO handle) noexcept : handle_(handle) {}
  ~DeviceInfoSet() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      SetupDiDestroyDeviceInfoList(handle_);
    }
  }
  DeviceInfoSet(const DeviceInfoSet&) = delete;
  DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }

 private:
  HDEVINFO handle_{INVALID_HANDLE_VALUE};
};

bool native_host_is_amd64(DWORD& native_code) noexcept {
  USHORT process_machine{};
  USHORT native_machine{};
  if (!IsWow64Process2(GetCurrentProcess(), &process_machine, &native_machine)) {
    native_code = GetLastError();
    return false;
  }
  native_code = ERROR_SUCCESS;
  return native_machine == IMAGE_FILE_MACHINE_AMD64;
}

void append_unique_name(
    std::vector<std::wstring>& names,
    const std::wstring& name) {
  if (name.empty()) {
    return;
  }
  const bool duplicate = std::any_of(names.begin(), names.end(), [&](const auto& item) {
    return equal_text_case_insensitive(item, name);
  });
  if (!duplicate) {
    names.push_back(name);
  }
}

bool evidence_matches_item(
    const DriverCandidateEvidence& evidence,
    const DriverInjectionItem& item) {
  return evidence.candidate_id == item.candidate_id &&
         evidence.origin == item.origin && evidence.category == item.category &&
         equal_text_case_insensitive(
             evidence.inf_path.lexically_normal().native(),
             item.inf_path.lexically_normal().native()) &&
         equal_text_case_insensitive(
             evidence.package_root.lexically_normal().native(),
             item.package_root.lexically_normal().native()) &&
         equal_text_case_insensitive(
             evidence.catalog_path.lexically_normal().native(),
             item.catalog_path.lexically_normal().native()) &&
         evidence.inf_sha256 == item.inf_sha256 &&
         evidence.package_tree_sha256 == item.package_tree_sha256 &&
         evidence.package_total_bytes == item.package_total_bytes &&
         evidence.package_file_count == item.package_file_count &&
         evidence.signer == item.signer &&
         evidence.architecture == DriverArchitectureState::amd64_verified &&
         evidence.signature == DriverSignatureState::trusted_signed &&
         evidence.path_state == DriverPathState::verified_regular_tree;
}

}  // namespace

DriverDiscoveryReport discover_current_pc_driver_candidates() {
  DriverDiscoveryReport report{
      .origin = DriverOrigin::current_pc,
  };
  DWORD architecture_error{};
  if (!native_host_is_amd64(architecture_error)) {
    append_diagnostic(
        report,
        "DRIVER_HOST_NOT_NATIVE_AMD64",
        {},
        L"現在PCのネイティブアーキテクチャをx64と確認できません",
        architecture_error);
    return report;
  }

  const auto windows = windows_directory();
  if (!windows.has_value()) {
    append_diagnostic(
        report,
        "DRIVER_WINDOWS_ROOT_UNKNOWN",
        {},
        L"Windowsフォルダーを確認できません",
        GetLastError());
    return report;
  }
  auto driver_store_root = inspect_directory(
      *windows / L"System32" / L"DriverStore" / L"FileRepository");
  if (!driver_store_root) {
    report.diagnostics.push_back(diagnostic_from_error(
        "DRIVER_STORE_ROOT_UNSAFE",
        *windows / L"System32" / L"DriverStore" / L"FileRepository",
        driver_store_root.error()));
    return report;
  }
  report.inspected_root = driver_store_root.value();

  DeviceInfoSet devices(SetupDiGetClassDevsW(
      nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
  if (!devices.valid()) {
    append_diagnostic(
        report,
        "DRIVER_PNP_ENUMERATION_FAILED",
        {},
        L"現在PCのPnPデバイスを読取り専用で列挙できません",
        GetLastError());
    return report;
  }

  std::map<std::wstring, DriverCandidateEvidence> unique;
  for (DWORD index = 0U;; ++index) {
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiEnumDeviceInfo(devices.get(), index, &device)) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_NO_MORE_ITEMS) {
        break;
      }
      append_diagnostic(
          report,
          "DRIVER_PNP_DEVICE_QUERY_FAILED",
          {},
          L"PnPデバイス情報の列挙を完了できません",
          native_code);
      return report;
    }
    const auto category = category_for_guid(device.ClassGuid);
    if (!category.has_value()) {
      continue;
    }
    const std::wstring published_name = query_device_property_string(
        devices.get(), device, kDriverInfPathProperty);
    if (published_name.empty()) {
      append_diagnostic(
          report,
          "DRIVER_PNP_INF_UNKNOWN",
          {},
          L"関連PnPデバイスの使用中INFを確認できないため除外しました",
          ERROR_NOT_FOUND,
          DiagnosticSeverity::warning);
      continue;
    }
    auto store_inf = resolve_driver_store_inf(published_name);
    if (!store_inf) {
      report.diagnostics.push_back(diagnostic_from_error(
          "DRIVER_STORE_RESOLUTION_FAILED", {}, store_inf.error()));
      continue;
    }

    std::wstring name = query_device_registry_string(
        devices.get(), device, SPDRP_FRIENDLYNAME);
    if (name.empty()) {
      name = query_device_registry_string(
          devices.get(), device, SPDRP_DEVICEDESC);
    }
    const std::wstring provider = query_device_property_string(
        devices.get(), device, kDriverProviderProperty);
    const std::wstring key = lower_text(
        store_inf.value().lexically_normal().native());
    const auto existing = unique.find(key);
    if (existing != unique.end()) {
      append_unique_name(existing->second.present_device_names, name);
      continue;
    }

    std::vector<std::wstring> names;
    append_unique_name(names, name);
    auto evidence = inspect_inf_candidate(
        DriverOrigin::current_pc,
        *category,
        name.empty() ? std::filesystem::path(published_name).filename().native()
                     : name,
        provider,
        std::move(names),
        store_inf.value(),
        DriverArchitectureState::amd64_verified,
        report.inspected_root);
    if (!evidence) {
      report.diagnostics.push_back(diagnostic_from_error(
          "DRIVER_CURRENT_PC_INSPECTION_FAILED",
          store_inf.value(),
          evidence.error()));
      continue;
    }
    unique.emplace(key, evidence.take_value());
  }

  report.candidates.reserve(unique.size());
  for (auto& [unused, evidence] : unique) {
    (void)unused;
    report.candidates.push_back(evaluate_driver_candidate(std::move(evidence)));
  }
  report.completed = true;
  return report;
}

DriverDiscoveryReport discover_manufacturer_driver_candidates(
    const std::filesystem::path& manufacturer_inf_root) {
  DriverDiscoveryReport report{
      .origin = DriverOrigin::manufacturer_folder,
      .inspected_root = manufacturer_inf_root,
  };
  if (manufacturer_inf_root.empty() || !manufacturer_inf_root.is_absolute()) {
    append_diagnostic(
        report,
        "DRIVER_MANUFACTURER_ROOT_INVALID",
        manufacturer_inf_root,
        L"メーカーINFフォルダーは絶対パスで指定してください",
        ERROR_INVALID_PARAMETER);
    return report;
  }
  auto inspected_root = inspect_directory(manufacturer_inf_root);
  if (!inspected_root) {
    report.diagnostics.push_back(diagnostic_from_error(
        "DRIVER_MANUFACTURER_ROOT_UNSAFE",
        manufacturer_inf_root,
        inspected_root.error()));
    return report;
  }
  report.inspected_root = inspected_root.value();

  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      report.inspected_root,
      std::filesystem::directory_options::none,
      error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) {
    append_diagnostic(
        report,
        "DRIVER_MANUFACTURER_ENUMERATION_FAILED",
        report.inspected_root,
        L"メーカーINFフォルダーを列挙できません",
        static_cast<DWORD>(error.value()));
    return report;
  }

  while (iterator != end) {
    const std::filesystem::path path = iterator->path();
    const auto status = iterator->symlink_status(error);
    if (error) {
      append_diagnostic(
          report,
          "DRIVER_MANUFACTURER_ENTRY_QUERY_FAILED",
          path,
          L"メーカーINFフォルダー内の属性を確認できません",
          static_cast<DWORD>(error.value()));
      return report;
    }
    if (std::filesystem::is_symlink(status) ||
        std::filesystem::is_other(status)) {
      if (std::filesystem::is_directory(status)) {
        iterator.disable_recursion_pending();
      }
      append_diagnostic(
          report,
          "DRIVER_MANUFACTURER_REPARSE_REJECTED",
          path,
          L"リンク／再解析ポイントを検出したため安全側に除外しました",
          ERROR_REPARSE_TAG_INVALID,
          DiagnosticSeverity::warning);
    } else if (std::filesystem::is_regular_file(status) &&
               equal_text_case_insensitive(path.extension().native(), L".inf")) {
      GUID class_guid{};
      std::array<wchar_t, kMaximumClassNameCharacters> class_name{};
      DWORD required{};
      if (!SetupDiGetINFClassW(
              path.c_str(),
              &class_guid,
              class_name.data(),
              static_cast<DWORD>(class_name.size()),
              &required)) {
        append_diagnostic(
            report,
            "DRIVER_INF_CLASS_UNKNOWN",
            path,
            L"INFのデバイスクラスを確認できないため除外しました",
            GetLastError(),
            DiagnosticSeverity::warning);
      } else {
        const auto category = category_for_guid(class_guid);
        if (category.has_value()) {
          const DriverArchitectureState architecture =
              query_inf_amd64_architecture(path);
          auto evidence = inspect_inf_candidate(
              DriverOrigin::manufacturer_folder,
              *category,
              path.filename().native(),
              {},
              {},
              path,
              architecture,
              report.inspected_root);
          if (!evidence) {
            report.diagnostics.push_back(diagnostic_from_error(
                "DRIVER_MANUFACTURER_INF_INSPECTION_FAILED",
                path,
                evidence.error()));
          } else {
            report.candidates.push_back(
                evaluate_driver_candidate(evidence.take_value()));
          }
        }
      }
    }
    iterator.increment(error);
    if (error) {
      append_diagnostic(
          report,
          "DRIVER_MANUFACTURER_ENUMERATION_INCOMPLETE",
          path,
          L"メーカーINFフォルダーの列挙を完了できません",
          static_cast<DWORD>(error.value()));
      return report;
    }
  }
  std::sort(
      report.candidates.begin(),
      report.candidates.end(),
      [](const auto& left, const auto& right) {
        return lower_text(left.evidence.inf_path.native()) <
               lower_text(right.evidence.inf_path.native());
      });
  report.completed = true;
  return report;
}

clonecore::Status revalidate_driver_injection_plan(
    const DriverInjectionPlan& plan) {
  if (plan.items().empty() || plan.items().size() != plan.commands().size()) {
    return clonecore::Status::failure(driver_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ドライバー注入計画再検証",
        L"注入計画の項目数が不正です"));
  }
  auto mount_root = inspect_directory(plan.mounted_image_root());
  if (!mount_root) {
    return clonecore::Status::failure(mount_root.error());
  }
  if (!equal_text_case_insensitive(
          mount_root.value().lexically_normal().native(),
          plan.mounted_image_root().lexically_normal().native())) {
    return clonecore::Status::failure(driver_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_BAD_PATHNAME,
        L"WIMマウント先最終パス再検証",
        L"WIMマウント先がリンクまたは差替えで別の場所を指しています"));
  }

  auto dism_file = inspect_and_hash_file(plan.dism_path());
  if (!dism_file ||
      !equal_text_case_insensitive(
          dism_file.value().canonical_path.lexically_normal().native(),
          plan.dism_path().lexically_normal().native())) {
    return clonecore::Status::failure(driver_error(
        clonecore::ErrorCode::identity_mismatch,
        dism_file ? ERROR_BAD_PATHNAME : dism_file.error().native_code,
        L"ドライバー注入DISM最終パス再検証",
        L"ADK DISMが通常ファイルではないか、別の場所を指しています"));
  }

  auto adk_environment = make_windows_adk_environment();
  const auto dism_kind = adk_environment->classify_path(plan.dism_path());
  if (!dism_kind || dism_kind.value() != PathKind::regular_file) {
    return clonecore::Status::failure(driver_error(
        clonecore::ErrorCode::verification_failed,
        dism_kind ? ERROR_INVALID_DATA : dism_kind.error().native_code,
        L"ドライバー注入DISM再検証",
        L"計画したADK DISMが通常ファイルではありません"));
  }
  const auto dism_trust =
      adk_environment->verify_microsoft_signed_executable(plan.dism_path());
  if (!dism_trust) {
    return clonecore::Status::failure(dism_trust.error());
  }

  std::optional<DriverDiscoveryReport> current_report;
  for (std::size_t index = 0; index < plan.items().size(); ++index) {
    const auto& item = plan.items()[index];
    const auto& command = plan.commands()[index];
    if (command.executable != plan.dism_path() ||
        command.arguments.size() != 4U ||
        command.arguments[0] != L"/English" ||
        command.arguments[1] !=
            L"/Image:" + plan.mounted_image_root().native() ||
        command.arguments[2] != L"/Add-Driver" ||
        command.arguments[3] != L"/Driver:" + item.inf_path.native()) {
      return clonecore::Status::failure(driver_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ドライバー注入コマンド再検証",
          L"注入コマンドが封印した項目と一致しません"));
    }

    if (item.origin == DriverOrigin::current_pc) {
      if (!current_report.has_value()) {
        current_report = discover_current_pc_driver_candidates();
      }
      if (!current_report->completed) {
        return clonecore::Status::failure(driver_error(
            clonecore::ErrorCode::enumeration_failed,
            ERROR_GEN_FAILURE,
            L"現在PCドライバー再列挙",
            L"現在PCのドライバーを再列挙できません"));
      }
      const auto found = std::find_if(
          current_report->candidates.begin(),
          current_report->candidates.end(),
          [&](const auto& candidate) {
            return candidate.evidence.candidate_id == item.candidate_id;
          });
      if (found == current_report->candidates.end() || !found->selectable ||
          !evidence_matches_item(found->evidence, item)) {
        return clonecore::Status::failure(driver_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"現在PCドライバー再検証",
            L"選択後にPnP割当、パス、内容、x64互換性、または署名が変化しました"));
      }
    } else {
      const DriverArchitectureState architecture =
          query_inf_amd64_architecture(item.inf_path);
      auto evidence = inspect_inf_candidate(
          item.origin,
          item.category,
          item.inf_path.filename().native(),
          {},
          {},
          item.inf_path,
          architecture,
          item.package_root);
      if (!evidence || !evidence_matches_item(evidence.value(), item)) {
        return clonecore::Status::failure(driver_error(
            clonecore::ErrorCode::identity_mismatch,
            evidence ? ERROR_FILE_INVALID : evidence.error().native_code,
            L"メーカーINFドライバー再検証",
            L"選択後にパス、内容、x64互換性、または署名が変化しました"));
      }
    }
  }
  return clonecore::success_status();
}

}  // namespace ytec::mediabuilder

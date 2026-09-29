#include "ytec/imageformat/restore_image_inspection.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/image_inspection.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::size_t kVerificationBlockBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumSingleReadBytes = 128U * 1024U * 1024U;

clonecore::Error inspection_error(
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

bool has_dcimg_extension(const std::wstring_view path) {
  constexpr std::wstring_view extension = L".dcimg";
  if (path.size() < extension.size()) {
    return false;
  }
  const std::wstring_view suffix =
      path.substr(path.size() - extension.size());
  return std::equal(
      suffix.begin(),
      suffix.end(),
      extension.begin(),
      [](const wchar_t left, const wchar_t right) {
        return std::towlower(left) == std::towlower(right);
      });
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& requested_path) {
  if (requested_path.empty() ||
      requested_path.size() >= 32767U ||
      !has_dcimg_extension(requested_path)) {
    return clonecore::Result<std::wstring>::failure(
        inspection_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"dcimg入力パス",
            L".dcimgのローカル絶対パスを指定してください"));
  }

  const DWORD required =
      GetFullPathNameW(requested_path.c_str(), 0, nullptr, nullptr);
  if (required == 0 || required >= 32767U) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"GetFullPathNameW(dcimg)",
            GetLastError()));
  }
  std::vector<wchar_t> buffer(required, L'\0');
  const DWORD written = GetFullPathNameW(
      requested_path.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (written == 0 ||
      static_cast<std::size_t>(written) >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"GetFullPathNameW(dcimg)",
            GetLastError()));
  }
  std::wstring path(buffer.data(), written);
  if (path.size() <= 3U ||
      std::iswalpha(path[0]) == 0 ||
      path[1] != L':' ||
      (path[2] != L'\\' && path[2] != L'/')) {
    return clonecore::Result<std::wstring>::failure(
        inspection_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_BAD_PATHNAME,
            L"dcimg入力パス",
            L"ネットワーク、デバイス、ドライブ直下は入力元にできません"));
  }
  return clonecore::Result<std::wstring>::success(std::move(path));
}

class WindowsRestoreImageReadSession final
    : public IRestoreImageReadSession {
 public:
  WindowsRestoreImageReadSession(
      std::wstring canonical_path,
      const std::uint64_t image_length,
      clonecore::UniqueHandle file)
      : canonical_path_(std::move(canonical_path)),
        image_length_(image_length),
        file_(std::move(file)) {}

  [[nodiscard]] const std::wstring& canonical_path()
      const noexcept override {
    return canonical_path_;
  }

  [[nodiscard]] std::uint64_t image_length()
      const noexcept override {
    return image_length_;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>>
  read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (length > kMaximumSingleReadBytes ||
        length > (std::numeric_limits<DWORD>::max)() ||
        offset > image_length_ ||
        length > image_length_ - offset ||
        offset >
            static_cast<std::uint64_t>(
                (std::numeric_limits<LONGLONG>::max)())) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          inspection_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"dcimg有界読取り",
              L"読取り範囲または一回の読取り長が上限外です"));
    }

    const std::scoped_lock lock(read_mutex_);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(
            file_.get(), position, nullptr, FILE_BEGIN)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"SetFilePointerEx(dcimg)",
              GetLastError()));
    }

    std::vector<std::byte> bytes(length);
    DWORD read{};
    if (length != 0 &&
        !ReadFile(
            file_.get(),
            bytes.data(),
            static_cast<DWORD>(length),
            &read,
            nullptr)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ReadFile(dcimg)",
              GetLastError()));
    }
    if (static_cast<std::size_t>(read) != length) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          inspection_error(
              clonecore::ErrorCode::io_failed,
              ERROR_HANDLE_EOF,
              L"ReadFile(dcimg)",
              L"要求した範囲を完全に読み取れませんでした"));
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

 private:
  std::wstring canonical_path_;
  std::uint64_t image_length_{};
  clonecore::UniqueHandle file_;
  mutable std::mutex read_mutex_;
};

}  // namespace

clonecore::Result<std::unique_ptr<IRestoreImageReadSession>>
open_restore_image_file_read_session_v1(
    const std::wstring& requested_path) {
  auto canonical = canonical_local_path(requested_path);
  if (!canonical) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        canonical.error());
  }

  const DWORD attributes =
      GetFileAttributesW(canonical.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"GetFileAttributesW(dcimg)",
            GetLastError()));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      (attributes & FILE_ATTRIBUTE_DEVICE) != 0U) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        inspection_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_REPARSE_TAG_INVALID,
            L"dcimg入力属性",
            L"通常ファイル以外またはreparse pointは入力元にできません"));
  }

  clonecore::UniqueHandle file(CreateFileW(
      canonical.value().c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_RANDOM_ACCESS,
      nullptr));
  if (!file.valid()) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            L"CreateFileW(dcimg read-only)",
            GetLastError()));
  }

  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (!GetFileInformationByHandleEx(
          file.get(),
          FileAttributeTagInfo,
          &tag_info,
          sizeof(tag_info))) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"GetFileInformationByHandleEx(dcimg)",
            GetLastError()));
  }
  if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      (tag_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        inspection_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_REPARSE_TAG_INVALID,
            L"dcimg入力ハンドル",
            L"オープン後の入力が通常ファイルではありません"));
  }

  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file.get(), &file_size)) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"GetFileSizeEx(dcimg)",
            GetLastError()));
  }
  if (file_size.QuadPart <= 0) {
    return clonecore::Result<
        std::unique_ptr<IRestoreImageReadSession>>::failure(
        inspection_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_HANDLE_EOF,
            L"GetFileSizeEx(dcimg)",
            L"空のファイルはdcimgとして検証できません"));
  }

  const std::uint64_t image_length =
      static_cast<std::uint64_t>(file_size.QuadPart);
  return clonecore::Result<
      std::unique_ptr<IRestoreImageReadSession>>::success(
      std::make_unique<WindowsRestoreImageReadSession>(
          canonical.take_value(), image_length, std::move(file)));
}

clonecore::Result<RestoreImageInspectionReport>
inspect_restore_image_reader_v1(
    std::wstring canonical_path,
    const std::uint64_t image_length,
    const Sha256ReadCallback& reader,
    const RestoreImageInspectionOptions& options) {
  const Sha256ReadCallback cancellable_reader =
      [&reader, &options](
          const std::uint64_t offset,
          const std::size_t length)
      -> clonecore::Result<std::vector<std::byte>> {
    if (options.cancellation_requested &&
        options.cancellation_requested()) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          inspection_error(
              clonecore::ErrorCode::access_denied,
              ERROR_CANCELLED,
              L"dcimg読取り専用検証キャンセル",
              L"利用者の要求により検証を中止しました"));
    }
    return reader(offset, length);
  };
  auto container = inspect_dcimg_v1_from_reader(
      image_length,
      kVerificationBlockBytes,
      cancellable_reader,
      DcimgReadInspectionCallbacks{
          .progress = options.progress,
      });
  if (!container) {
    return clonecore::Result<
        RestoreImageInspectionReport>::failure(container.error());
  }

  auto metadata = inspect_dcimg_metadata_v1(
      container.value().container.header,
      container.value().manifest,
      container.value().partition_table_snapshot);
  if (!metadata) {
    return clonecore::Result<
        RestoreImageInspectionReport>::failure(metadata.error());
  }
  const auto layout = validate_dcimg_restore_layout_v1(
      container.value().container, metadata.value());
  if (!layout) {
    return clonecore::Result<
        RestoreImageInspectionReport>::failure(layout.error());
  }

  return clonecore::Result<RestoreImageInspectionReport>::success(
      RestoreImageInspectionReport{
          .canonical_path = std::move(canonical_path),
          .image_length = image_length,
          .global_hash = container.value().container.global_hash,
          .header = container.value().container.header,
          .manifest = std::move(metadata.value().manifest),
          .partition_snapshot =
              std::move(metadata.value().partition_snapshot),
          .complete_container_verified = true,
          .metadata_verified = true,
          .restore_layout_verified = true,
          .restore_execution_enabled = false,
      });
}

clonecore::Result<RestoreImageInspectionReport>
inspect_restore_image_file_v1(
    const std::wstring& requested_path,
    const RestoreImageInspectionOptions& options) {
  auto session =
      open_restore_image_file_read_session_v1(requested_path);
  if (!session) {
    return clonecore::Result<
        RestoreImageInspectionReport>::failure(session.error());
  }
  const std::wstring canonical = session.value()->canonical_path();
  const std::uint64_t image_length =
      session.value()->image_length();
  const Sha256ReadCallback reader =
      [&session](
          const std::uint64_t offset,
          const std::size_t length) {
        return session.value()->read_at(offset, length);
      };
  return inspect_restore_image_reader_v1(
      canonical, image_length, reader, options);
}

}  // namespace ytec::imageformat

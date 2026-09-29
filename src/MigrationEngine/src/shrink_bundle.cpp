#include "ytec/migrationengine/shrink_bundle.h"

#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

constexpr std::size_t kHashBlockBytes = 4U * 1024U * 1024U;

clonecore::Error bundle_error(
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
  return clonecore::Result<T>::failure(bundle_error(
      code, native_code, std::move(operation), std::move(message)));
}

std::wstring lower_copy(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t value) {
    return static_cast<wchar_t>(std::towlower(value));
  });
  return value;
}

clonecore::Result<std::wstring> utf8_name_to_wide(const std::string& value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"縮小イメージのファイル名変換",
        L"WIMファイル名が不正です");
  }
  const int length = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (length <= 0) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"縮小イメージのファイル名変換",
        L"WIMファイル名が正しいUTF-8ではありません");
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          length) != length) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"縮小イメージのファイル名変換",
        L"WIMファイル名を完全に変換できません");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

clonecore::Result<std::uint64_t> regular_file_length(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        GetLastError(),
        std::wstring(operation),
        L"ファイル属性を取得できません");
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常ファイルではないか、reparse pointです");
  }
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) ||
      standard.EndOfFile.QuadPart < 0) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        GetLastError(),
        std::wstring(operation),
        L"ファイル長を取得できません");
  }
  return clonecore::Result<std::uint64_t>::success(
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart));
}

clonecore::Result<std::vector<std::byte>> read_exact(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::size_t length,
    const std::wstring_view operation) {
  if (length > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) ||
      offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"読み取り範囲が大きすぎます");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed, operation, GetLastError()));
  }
  std::vector<std::byte> bytes(length);
  DWORD transferred = 0;
  if (length != 0U &&
      (!ReadFile(
           handle,
           bytes.data(),
           static_cast<DWORD>(length),
           &transferred,
           nullptr) ||
       transferred != length)) {
    const DWORD native_code = GetLastError();
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::io_failed,
        native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code,
        std::wstring(operation),
        L"ファイルを要求長どおり読み取れません");
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Result<imageformat::Sha256Digest> hash_locked_file(
    const HANDLE handle,
    const std::uint64_t length,
    const std::wstring_view operation) {
  return imageformat::sha256_from_reader(
      length,
      kHashBlockBytes,
      [handle, operation](const std::uint64_t offset, const std::size_t bytes) {
        return read_exact(handle, offset, bytes, operation);
      });
}

clonecore::Result<clonecore::UniqueHandle> open_locked_regular_file(
    const std::wstring& path,
    const std::wstring_view operation) {
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!handle) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::verification_failed,
            operation,
            GetLastError()));
  }
  const auto length = regular_file_length(handle.get(), operation);
  if (!length) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(length.error());
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(std::move(handle));
}

clonecore::Result<clonecore::UniqueHandle> open_locked_directory(
    const std::wstring& path) {
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::verification_failed,
            L"縮小イメージ束フォルダー固定",
            GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          handle.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    const DWORD native_code = GetLastError();
    return failure<clonecore::UniqueHandle>(
        clonecore::ErrorCode::verification_failed,
        native_code == ERROR_SUCCESS ? ERROR_REPARSE_TAG_INVALID : native_code,
        L"縮小イメージ束フォルダー固定",
        L".dcmigは通常フォルダーでなければなりません");
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(std::move(handle));
}

clonecore::Status verify_declared_directory_entries(
    const std::wstring& directory,
    const std::set<std::wstring>& expected_names) {
  const std::wstring pattern = directory + L"\\*";
  WIN32_FIND_DATAW data{};
  HANDLE search = FindFirstFileW(pattern.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::verification_failed,
        L"縮小イメージ束内容列挙",
        GetLastError()));
  }
  std::set<std::wstring> actual;
  do {
    const std::wstring name(data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    if ((data.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
      FindClose(search);
      return clonecore::Status::failure(bundle_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"縮小イメージ束内容列挙",
          L"束内に通常ファイルではない項目があります"));
    }
    actual.insert(lower_copy(name));
  } while (FindNextFileW(search, &data));
  const DWORD enumeration_error = GetLastError();
  FindClose(search);
  if (enumeration_error != ERROR_NO_MORE_FILES) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::verification_failed,
        L"縮小イメージ束内容列挙",
        enumeration_error));
  }
  if (actual != expected_names) {
    return clonecore::Status::failure(bundle_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"縮小イメージ束内容照合",
        L"マニフェストにないファイル、または不足しているWIMがあります"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<LockedFileHash> hash_regular_file_locked_read_only(
    const std::wstring& absolute_path) {
  if (absolute_path.empty() ||
      !std::filesystem::path(absolute_path).is_absolute()) {
    return failure<LockedFileHash>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"固定ファイルSHA-256",
        L"絶対パスを指定してください");
  }
  auto handle = open_locked_regular_file(
      absolute_path, L"固定ファイル読取り専用オープン");
  if (!handle) {
    return clonecore::Result<LockedFileHash>::failure(handle.error());
  }
  const auto length = regular_file_length(
      handle.value().get(), L"固定ファイル長");
  if (!length) {
    return clonecore::Result<LockedFileHash>::failure(length.error());
  }
  const auto hash = hash_locked_file(
      handle.value().get(), length.value(), L"固定ファイルSHA-256");
  if (!hash) {
    return clonecore::Result<LockedFileHash>::failure(hash.error());
  }
  return clonecore::Result<LockedFileHash>::success(LockedFileHash{
      .absolute_path = absolute_path,
      .length_bytes = length.value(),
      .sha256 = hash.value(),
      .locked_read_handle = handle.take_value(),
  });
}

clonecore::Result<VerifiedShrinkBundle> verify_shrink_bundle_read_only(
    const std::wstring& manifest_path) {
  if (manifest_path.empty() || !std::filesystem::path(manifest_path).is_absolute()) {
    return failure<VerifiedShrinkBundle>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"縮小イメージ束検証",
        L"manifest.dcmigは絶対パスで指定してください");
  }
  const std::filesystem::path path(manifest_path);
  if (lower_copy(path.filename().wstring()) !=
      lower_copy(kShrinkBundleManifestFileName) ||
      lower_copy(path.parent_path().extension().wstring()) != L".dcmig") {
    return failure<VerifiedShrinkBundle>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"縮小イメージ束検証",
        L"manifest.dcmigを.dcmigフォルダー直下に置いてください");
  }
  const auto physical_disk =
      diskmodel::query_single_disk_number_for_local_path(manifest_path);
  if (!physical_disk) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(
        physical_disk.error());
  }
  const std::wstring directory = path.parent_path().wstring();
  auto directory_handle = open_locked_directory(directory);
  if (!directory_handle) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(
        directory_handle.error());
  }
  auto manifest_handle = open_locked_regular_file(
      manifest_path, L"縮小イメージマニフェスト固定");
  if (!manifest_handle) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(
        manifest_handle.error());
  }
  const auto manifest_length = regular_file_length(
      manifest_handle.value().get(), L"縮小イメージマニフェスト長");
  if (!manifest_length) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(
        manifest_length.error());
  }
  if (manifest_length.value() == 0U ||
      manifest_length.value() > imageformat::kMaximumShrinkImageManifestBytes) {
    return failure<VerifiedShrinkBundle>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"縮小イメージマニフェスト長",
        L"マニフェストの長さが許容範囲外です");
  }
  const auto manifest_bytes = read_exact(
      manifest_handle.value().get(),
      0,
      static_cast<std::size_t>(manifest_length.value()),
      L"縮小イメージマニフェスト読込");
  if (!manifest_bytes) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(
        manifest_bytes.error());
  }
  const auto manifest = imageformat::inspect_shrink_image_manifest_v1(
      manifest_bytes.value());
  if (!manifest) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(manifest.error());
  }
  const auto manifest_hash = imageformat::sha256(manifest_bytes.value());
  if (!manifest_hash) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(
        manifest_hash.error());
  }

  VerifiedShrinkBundle report{
      .bundle_directory = directory,
      .manifest_path = manifest_path,
      .manifest = manifest.value(),
      .manifest_length_bytes = manifest_length.value(),
      .manifest_sha256 = manifest_hash.value(),
      .locked_manifest_handle = manifest_handle.take_value(),
      .locked_directory_handle = directory_handle.take_value(),
  };
  std::set<std::wstring> expected_names{
      lower_copy(kShrinkBundleManifestFileName)};
  for (const auto& partition : report.manifest.partitions) {
    if (partition.payload_file_name.empty()) {
      continue;
    }
    auto wide_name = utf8_name_to_wide(partition.payload_file_name);
    if (!wide_name) {
      return clonecore::Result<VerifiedShrinkBundle>::failure(
          wide_name.error());
    }
    const std::wstring comparable_name = lower_copy(wide_name.value());
    if (!expected_names.insert(comparable_name).second) {
      return failure<VerifiedShrinkBundle>(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"縮小イメージ束内容照合",
          L"大文字小文字だけが異なる重複WIM名があります");
    }
    const std::wstring payload_path =
        (path.parent_path() / wide_name.value()).wstring();
    auto payload_handle = open_locked_regular_file(
        payload_path, L"縮小イメージWIM固定");
    if (!payload_handle) {
      return clonecore::Result<VerifiedShrinkBundle>::failure(
          payload_handle.error());
    }
    const auto length = regular_file_length(
        payload_handle.value().get(), L"縮小イメージWIM長");
    if (!length) {
      return clonecore::Result<VerifiedShrinkBundle>::failure(length.error());
    }
    if (length.value() != partition.payload_length_bytes) {
      return failure<VerifiedShrinkBundle>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"縮小イメージWIM長照合",
          L"WIMの長さがマニフェストと一致しません");
    }
    const auto hash = hash_locked_file(
        payload_handle.value().get(), length.value(), L"縮小イメージWIM SHA-256");
    if (!hash) {
      return clonecore::Result<VerifiedShrinkBundle>::failure(hash.error());
    }
    if (hash.value() != partition.payload_sha256) {
      return failure<VerifiedShrinkBundle>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"縮小イメージWIM SHA-256照合",
          L"WIMのSHA-256がマニフェストと一致しません");
    }
    report.payloads.push_back(VerifiedShrinkPayload{
        .file_name = partition.payload_file_name,
        .absolute_path = payload_path,
        .length_bytes = length.value(),
        .sha256 = hash.value(),
        .locked_read_handle = payload_handle.take_value(),
    });
  }
  const auto entries = verify_declared_directory_entries(directory, expected_names);
  if (!entries) {
    return clonecore::Result<VerifiedShrinkBundle>::failure(entries.error());
  }
  return clonecore::Result<VerifiedShrinkBundle>::success(std::move(report));
}

}  // namespace ytec::migrationengine

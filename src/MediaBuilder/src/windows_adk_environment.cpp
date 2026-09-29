#include "ytec/mediabuilder/adk_detection.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/error.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <Msi.h>
#include <winver.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace ytec::mediabuilder {
namespace {

constexpr wchar_t kInstalledRootsRegistryKey[] =
    L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots";
constexpr wchar_t kKitsRoot10Value[] = L"KitsRoot10";
constexpr wchar_t kAdkRelativePath[] =
    L"Windows Kits\\10\\Assessment and Deployment Kit";
constexpr DWORD kMaximumVersionResourceBytes = 1024U * 1024U;
constexpr DWORD kInitialMsiTextCharacters = 256U;
constexpr DWORD kMaximumMsiTextCharacters = 64U * 1024U;

std::optional<std::filesystem::path> read_environment_path(
    const wchar_t* name) {
  const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
  if (required == 0) {
    return std::nullopt;
  }

  std::vector<wchar_t> buffer(required, L'\0');
  const DWORD written =
      GetEnvironmentVariableW(name, buffer.data(), required);
  if (written == 0 || written >= required) {
    return std::nullopt;
  }
  return std::filesystem::path(std::wstring(buffer.data(), written));
}

std::optional<std::filesystem::path> read_kits_root10(
    DWORD registry_view_flag) {
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND |
                      registry_view_flag;
  DWORD value_type{};
  DWORD value_bytes{};
  const LSTATUS size_status = RegGetValueW(
      HKEY_LOCAL_MACHINE,
      kInstalledRootsRegistryKey,
      kKitsRoot10Value,
      flags,
      &value_type,
      nullptr,
      &value_bytes);
  if (size_status != ERROR_SUCCESS ||
      (value_type != REG_SZ && value_type != REG_EXPAND_SZ) ||
      value_bytes < sizeof(wchar_t) ||
      value_bytes > 32768U * sizeof(wchar_t)) {
    return std::nullopt;
  }

  std::vector<wchar_t> value(
      (value_bytes / sizeof(wchar_t)) + 1U, L'\0');
  DWORD read_bytes = value_bytes;
  const LSTATUS read_status = RegGetValueW(
      HKEY_LOCAL_MACHINE,
      kInstalledRootsRegistryKey,
      kKitsRoot10Value,
      flags,
      &value_type,
      value.data(),
      &read_bytes);
  if (read_status != ERROR_SUCCESS) {
    return std::nullopt;
  }

  value.back() = L'\0';
  std::wstring expanded;
  if (value_type == REG_EXPAND_SZ) {
    const DWORD required = ExpandEnvironmentStringsW(value.data(), nullptr, 0);
    if (required == 0 || required > 32768U) {
      return std::nullopt;
    }
    expanded.resize(required, L'\0');
    const DWORD written =
        ExpandEnvironmentStringsW(value.data(), expanded.data(), required);
    if (written == 0 || written > required) {
      return std::nullopt;
    }
    if (!expanded.empty() && expanded.back() == L'\0') {
      expanded.pop_back();
    }
  } else {
    expanded.assign(value.data());
  }

  if (expanded.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(expanded);
}

bool equal_path_case_insensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
  const std::wstring left_text = left.lexically_normal().native();
  const std::wstring right_text = right.lexically_normal().native();
  return CompareStringOrdinal(
             left_text.c_str(),
             static_cast<int>(left_text.size()),
             right_text.c_str(),
             static_cast<int>(right_text.size()),
             TRUE) == CSTR_EQUAL;
}

bool equal_text_case_insensitive(
    std::wstring_view left,
    std::wstring_view right) {
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

clonecore::Error msi_query_error(
    std::wstring context,
    UINT native_code) {
  return clonecore::make_win32_error(
      clonecore::ErrorCode::query_failed,
      std::move(context),
      native_code);
}

void append_unique(
    std::vector<std::filesystem::path>& paths,
    std::filesystem::path path) {
  if (path.empty()) {
    return;
  }
  path = path.lexically_normal();
  const bool duplicate = std::any_of(
      paths.begin(), paths.end(), [&path](const auto& existing) {
        return equal_path_case_insensitive(existing, path);
      });
  if (!duplicate) {
    paths.push_back(std::move(path));
  }
}

class WindowsAdkEnvironment final : public IAdkEnvironment {
 public:
  WindowsAdkEnvironment()
      : verifier_(bootrepair::make_windows_authenticode_verifier()) {}

  [[nodiscard]] clonecore::Result<PathKind> classify_path(
      const std::filesystem::path& path) override {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return clonecore::Result<PathKind>::success(PathKind::missing);
      }
      return clonecore::Result<PathKind>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"GetFileAttributesW(ADK)",
              native_code));
    }

    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Result<PathKind>::success(PathKind::reparse_point);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
      return clonecore::Result<PathKind>::success(PathKind::directory);
    }
    if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0U) {
      return clonecore::Result<PathKind>::success(PathKind::other);
    }
    return clonecore::Result<PathKind>::success(PathKind::regular_file);
  }

  [[nodiscard]] clonecore::Result<std::string> read_text_file(
      const std::filesystem::path& path,
      std::size_t maximum_bytes) override {
    clonecore::UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.valid()) {
      return clonecore::Result<std::string>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"CreateFileW(ADK command script)",
              GetLastError()));
    }

    FILE_ATTRIBUTE_TAG_INFO tag_info{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
      return clonecore::Result<std::string>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"GetFileInformationByHandleEx(ADK command script)",
              GetLastError()));
    }
    if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Result<std::string>::failure(clonecore::Error{
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"Read ADK command script",
          L"ADK command script is a reparse point",
      });
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size)) {
      return clonecore::Result<std::string>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"GetFileSizeEx(ADK command script)",
              GetLastError()));
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maximum_bytes ||
        static_cast<unsigned long long>(size.QuadPart) >
            std::numeric_limits<DWORD>::max()) {
      return clonecore::Result<std::string>::failure(clonecore::Error{
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_TOO_LARGE,
          L"Read ADK command script",
          L"ADK command script exceeds the safe size limit",
      });
    }

    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytes_read{};
    if (!contents.empty() &&
        !ReadFile(
            handle.get(),
            contents.data(),
            static_cast<DWORD>(contents.size()),
            &bytes_read,
            nullptr)) {
      return clonecore::Result<std::string>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ReadFile(ADK command script)",
              GetLastError()));
    }
    if (static_cast<std::size_t>(bytes_read) != contents.size()) {
      return clonecore::Result<std::string>::failure(clonecore::Error{
          clonecore::ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"Read ADK command script",
          L"ADK command script was not read completely",
      });
    }

    return clonecore::Result<std::string>::success(std::move(contents));
  }

  [[nodiscard]] clonecore::Status verify_microsoft_signed_executable(
      const std::filesystem::path& path) override {
    return verifier_->verify_microsoft_signed(path.native());
  }

  [[nodiscard]] clonecore::Result<FileVersion> query_file_version(
      const std::filesystem::path& path) override {
    DWORD ignored{};
    const DWORD required =
        GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (required == 0U || required > kMaximumVersionResourceBytes) {
      DWORD native_code = GetLastError();
      if (native_code == ERROR_SUCCESS) {
        native_code = required == 0U ? ERROR_RESOURCE_DATA_NOT_FOUND
                                     : ERROR_FILE_TOO_LARGE;
      }
      return clonecore::Result<FileVersion>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"GetFileVersionInfoSizeW(ADK)",
              native_code));
    }

    std::vector<std::byte> version_data(required);
    if (!GetFileVersionInfoW(
            path.c_str(),
            0,
            required,
            version_data.data())) {
      return clonecore::Result<FileVersion>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"GetFileVersionInfoW(ADK)",
              GetLastError()));
    }

    VS_FIXEDFILEINFO* fixed_info{};
    UINT fixed_info_bytes{};
    if (!VerQueryValueW(
            version_data.data(),
            L"\\",
            reinterpret_cast<void**>(&fixed_info),
            &fixed_info_bytes) ||
        fixed_info == nullptr ||
        fixed_info_bytes < sizeof(VS_FIXEDFILEINFO) ||
        fixed_info->dwSignature != VS_FFI_SIGNATURE) {
      return clonecore::Result<FileVersion>::failure(clonecore::Error{
          clonecore::ErrorCode::invalid_data,
          ERROR_RESOURCE_DATA_NOT_FOUND,
          L"VerQueryValueW(ADK)",
          L"ADK executable has no valid fixed file version",
      });
    }

    return clonecore::Result<FileVersion>::success(FileVersion{
        static_cast<std::uint16_t>(HIWORD(fixed_info->dwFileVersionMS)),
        static_cast<std::uint16_t>(LOWORD(fixed_info->dwFileVersionMS)),
        static_cast<std::uint16_t>(HIWORD(fixed_info->dwFileVersionLS)),
        static_cast<std::uint16_t>(LOWORD(fixed_info->dwFileVersionLS)),
    });
  }

  [[nodiscard]] clonecore::Result<std::wstring>
  query_msi_product_version(std::wstring_view product_code) override {
    std::wstring product(product_code);
    std::vector<wchar_t> value(kInitialMsiTextCharacters, L'\0');
    DWORD characters = static_cast<DWORD>(value.size());
    UINT status = MsiGetProductInfoW(
        product.c_str(),
        INSTALLPROPERTY_VERSIONSTRING,
        value.data(),
        &characters);
    if (status == ERROR_MORE_DATA) {
      if (characters >= kMaximumMsiTextCharacters) {
        return clonecore::Result<std::wstring>::failure(
            msi_query_error(
                L"MsiGetProductInfoW(ADK version)",
                ERROR_INSUFFICIENT_BUFFER));
      }
      value.assign(static_cast<std::size_t>(characters) + 1U, L'\0');
      characters = static_cast<DWORD>(value.size());
      status = MsiGetProductInfoW(
          product.c_str(),
          INSTALLPROPERTY_VERSIONSTRING,
          value.data(),
          &characters);
    }
    if (status != ERROR_SUCCESS) {
      return clonecore::Result<std::wstring>::failure(
          msi_query_error(L"MsiGetProductInfoW(ADK version)", status));
    }

    return clonecore::Result<std::wstring>::success(
        std::wstring(value.data(), characters));
  }

  [[nodiscard]] clonecore::Result<bool> is_msi_patch_applied(
      std::wstring_view product_code,
      std::wstring_view patch_code) override {
    std::wstring product(product_code);
    for (DWORD index = 0;; ++index) {
      wchar_t found_patch[39]{};
      std::vector<wchar_t> transforms(
          kInitialMsiTextCharacters, L'\0');
      DWORD characters = static_cast<DWORD>(transforms.size());
      UINT status = MsiEnumPatchesW(
          product.c_str(),
          index,
          found_patch,
          transforms.data(),
          &characters);
      if (status == ERROR_MORE_DATA) {
        if (characters >= kMaximumMsiTextCharacters) {
          return clonecore::Result<bool>::failure(msi_query_error(
              L"MsiEnumPatchesW(ADK servicing)",
              ERROR_INSUFFICIENT_BUFFER));
        }
        transforms.assign(
            static_cast<std::size_t>(characters) + 1U, L'\0');
        characters = static_cast<DWORD>(transforms.size());
        status = MsiEnumPatchesW(
            product.c_str(),
            index,
            found_patch,
            transforms.data(),
            &characters);
      }
      if (status == ERROR_NO_MORE_ITEMS) {
        return clonecore::Result<bool>::success(false);
      }
      if (status != ERROR_SUCCESS) {
        return clonecore::Result<bool>::failure(
            msi_query_error(
                L"MsiEnumPatchesW(ADK servicing)", status));
      }
      if (equal_text_case_insensitive(found_patch, patch_code)) {
        return clonecore::Result<bool>::success(true);
      }
      if (index == std::numeric_limits<DWORD>::max()) {
        return clonecore::Result<bool>::failure(msi_query_error(
            L"MsiEnumPatchesW(ADK servicing)", ERROR_ARITHMETIC_OVERFLOW));
      }
    }
  }

 private:
  std::unique_ptr<bootrepair::IExecutableTrustVerifier> verifier_;
};

}  // namespace

std::vector<std::filesystem::path> windows_adk_candidate_roots() {
  std::vector<std::filesystem::path> roots;

  if (const auto program_files_x86 =
          read_environment_path(L"ProgramFiles(x86)")) {
    append_unique(roots, *program_files_x86 / kAdkRelativePath);
  }
  if (const auto program_files = read_environment_path(L"ProgramFiles")) {
    append_unique(roots, *program_files / kAdkRelativePath);
  }

  for (const DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
    if (const auto kits_root = read_kits_root10(view)) {
      append_unique(roots, *kits_root / L"Assessment and Deployment Kit");
    }
  }

  append_unique(
      roots,
      std::filesystem::path(L"C:\\Program Files (x86)") / kAdkRelativePath);
  append_unique(
      roots,
      std::filesystem::path(L"C:\\Program Files") / kAdkRelativePath);
  return roots;
}

std::unique_ptr<IAdkEnvironment> make_windows_adk_environment() {
  return std::make_unique<WindowsAdkEnvironment>();
}

}  // namespace ytec::mediabuilder

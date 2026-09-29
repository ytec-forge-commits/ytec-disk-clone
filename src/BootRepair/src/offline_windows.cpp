#include "ytec/bootrepair/offline_windows.h"

#include "ytec/bootrepair/registry_hive.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::uint16_t kDosSignature = 0x5A4D;
constexpr std::uint32_t kPeSignature = 0x00004550;
constexpr std::uint16_t kMachineI386 = 0x014C;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kMachineArm64 = 0xAA64;
constexpr std::uint16_t kOptionalHeaderPe32 = 0x010B;
constexpr std::uint16_t kOptionalHeaderPe32Plus = 0x020B;
constexpr std::size_t kDosPeOffsetField = 0x3C;
constexpr std::size_t kCoffHeaderBytes = 20;
constexpr std::size_t kPeSignatureBytes = 4;
constexpr std::size_t kMachineOffset = kPeSignatureBytes;
constexpr std::size_t kOptionalHeaderSizeOffset =
    kPeSignatureBytes + 16;
constexpr std::size_t kOptionalHeaderOffset =
    kPeSignatureBytes + kCoffHeaderBytes;
constexpr std::size_t kMaximumHeaderPrefixBytes = 64U * 1024U;
constexpr std::uint64_t kMaximumKernelBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kWindows10InitialBuild = 10240U;
constexpr std::size_t kMaximumRegistryTextCharacters = 128;
constexpr std::size_t kMaximumWindowsPathCharacters = 32768;

using OrHandle = void*;
using OrOpenHiveFunction = DWORD(WINAPI*)(PCWSTR, OrHandle*);
using OrGetValueFunction = DWORD(WINAPI*)(
    OrHandle, PCWSTR, PCWSTR, PDWORD, PVOID, PDWORD);
using OrCloseHiveFunction = DWORD(WINAPI*)(OrHandle);

clonecore::Error make_error(
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

class UniqueLibrary final {
 public:
  explicit UniqueLibrary(HMODULE module = nullptr) noexcept : module_(module) {}

  ~UniqueLibrary() {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  UniqueLibrary(const UniqueLibrary&) = delete;
  UniqueLibrary& operator=(const UniqueLibrary&) = delete;

  [[nodiscard]] HMODULE get() const noexcept { return module_; }

 private:
  HMODULE module_{};
};

class UniqueOfflineHive final {
 public:
  UniqueOfflineHive(
      OrHandle handle,
      const OrCloseHiveFunction close_function) noexcept
      : handle_(handle), close_function_(close_function) {}

  ~UniqueOfflineHive() {
    if (handle_ != nullptr && close_function_ != nullptr) {
      (void)close_function_(handle_);
    }
  }

  UniqueOfflineHive(const UniqueOfflineHive&) = delete;
  UniqueOfflineHive& operator=(const UniqueOfflineHive&) = delete;

  [[nodiscard]] OrHandle get() const noexcept { return handle_; }

  [[nodiscard]] DWORD close() noexcept {
    if (handle_ == nullptr || close_function_ == nullptr) {
      return ERROR_INVALID_HANDLE;
    }
    const DWORD status = close_function_(handle_);
    if (status == ERROR_SUCCESS) {
      handle_ = nullptr;
    }
    return status;
  }

 private:
  OrHandle handle_{};
  OrCloseHiveFunction close_function_{};
};

template <typename Function>
Function load_offline_registry_function(
    const HMODULE module,
    const char* const function_name) noexcept {
#pragma warning(push)
#pragma warning(disable : 4191)
  const auto function =
      reinterpret_cast<Function>(GetProcAddress(module, function_name));
#pragma warning(pop)
  return function;
}

clonecore::Result<std::wstring> system32_file_path(
    const std::wstring_view file_name) {
  std::vector<wchar_t> buffer(kMaximumWindowsPathCharacters, L'\0');
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    DWORD native_code = GetLastError();
    if (native_code == ERROR_SUCCESS) {
      native_code = ERROR_INSUFFICIENT_BUFFER;
    }
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"System32パス取得",
            native_code));
  }
  std::wstring path(buffer.data(), length);
  if (path.empty() || (path.back() != L'\\' && path.back() != L'/')) {
    path.push_back(L'\\');
  }
  path.append(file_name);
  return clonecore::Result<std::wstring>::success(std::move(path));
}

clonecore::Status verify_loaded_module_path(
    const HMODULE module,
    const std::wstring& expected_path) {
  std::vector<wchar_t> buffer(kMaximumWindowsPathCharacters, L'\0');
  const DWORD length = GetModuleFileNameW(
      module, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    DWORD native_code = GetLastError();
    if (native_code == ERROR_SUCCESS) {
      native_code = ERROR_INSUFFICIENT_BUFFER;
    }
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"WindowsオフラインレジストリDLLパス取得",
        native_code));
  }
  const std::wstring actual_path(buffer.data(), length);
  if (_wcsicmp(actual_path.c_str(), expected_path.c_str()) != 0) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DLL,
        L"WindowsオフラインレジストリDLL検証",
        L"System32外のDLLは読み込みません"));
  }
  return clonecore::success_status();
}

clonecore::Status verify_regular_file(
    const std::wstring& path,
    const std::wstring& operation) {
  clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        operation,
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (!GetFileInformationByHandleEx(
          file.get(),
          FileAttributeTagInfo,
          &tag_info,
          sizeof(tag_info))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        operation,
        GetLastError()));
  }
  if ((tag_info.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        operation,
        L"ディレクトリまたはreparse pointは使用できません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint32_t> parse_build_number(
    const std::wstring& text) {
  if (text.empty() || text.size() > 10) {
    return clonecore::Result<std::uint32_t>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オフラインWindowsビルド番号解析",
        L"ビルド番号の長さが不正です"));
  }
  std::array<char, 10> ascii{};
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] < L'0' || text[index] > L'9') {
      return clonecore::Result<std::uint32_t>::failure(make_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オフラインWindowsビルド番号解析",
          L"ビルド番号に数字以外が含まれています"));
    }
    ascii[index] = static_cast<char>(text[index]);
  }
  std::uint32_t build{};
  const auto parsed = std::from_chars(
      ascii.data(), ascii.data() + text.size(), build);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != ascii.data() + text.size()) {
    return clonecore::Result<std::uint32_t>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"オフラインWindowsビルド番号解析",
        L"ビルド番号を安全に変換できません"));
  }
  return clonecore::Result<std::uint32_t>::success(build);
}

clonecore::Result<std::wstring> query_offline_registry_text(
    const OrGetValueFunction get_value,
    const OrHandle hive,
    const wchar_t* const value_name) {
  constexpr wchar_t kVersionKey[] =
      L"Microsoft\\Windows NT\\CurrentVersion";
  DWORD type{};
  DWORD bytes{};
  DWORD status = get_value(
      hive, kVersionKey, value_name, &type, nullptr, &bytes);
  if (status != ERROR_SUCCESS) {
    return clonecore::Result<std::wstring>::failure(make_error(
        clonecore::ErrorCode::query_failed,
        status,
        L"オフラインWindowsバージョン照会",
        L"必須レジストリ文字列を取得できません"));
  }
  if ((type != REG_SZ && type != REG_EXPAND_SZ) ||
      bytes < sizeof(wchar_t) ||
      bytes > kMaximumRegistryTextCharacters * sizeof(wchar_t) ||
      bytes % sizeof(wchar_t) != 0) {
    return clonecore::Result<std::wstring>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オフラインWindowsバージョン照会",
        L"必須レジストリ文字列の型または長さが不正です"));
  }

  std::vector<wchar_t> buffer(
      bytes / sizeof(wchar_t) + 1U, L'\0');
  DWORD buffer_bytes = static_cast<DWORD>(
      buffer.size() * sizeof(wchar_t));
  status = get_value(
      hive,
      kVersionKey,
      value_name,
      &type,
      buffer.data(),
      &buffer_bytes);
  if (status != ERROR_SUCCESS) {
    return clonecore::Result<std::wstring>::failure(make_error(
        clonecore::ErrorCode::query_failed,
        status,
        L"オフラインWindowsバージョン照会",
        L"必須レジストリ文字列を読み取れません"));
  }
  if ((type != REG_SZ && type != REG_EXPAND_SZ) ||
      buffer_bytes < sizeof(wchar_t) ||
      buffer_bytes > buffer.size() * sizeof(wchar_t) ||
      buffer_bytes % sizeof(wchar_t) != 0) {
    return clonecore::Result<std::wstring>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オフラインWindowsバージョン照会",
        L"必須レジストリ文字列の読取り結果が不正です"));
  }
  buffer.resize(buffer_bytes / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  if (buffer.empty() ||
      std::find(buffer.begin(), buffer.end(), L'\0') != buffer.end()) {
    return clonecore::Result<std::wstring>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オフラインWindowsバージョン照会",
        L"必須レジストリ文字列が空または途中で終端されています"));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(buffer.begin(), buffer.end()));
}

clonecore::Result<std::uint32_t> query_offline_registry_dword(
    const OrGetValueFunction get_value,
    const OrHandle hive,
    const wchar_t* const value_name) {
  constexpr wchar_t kVersionKey[] =
      L"Microsoft\\Windows NT\\CurrentVersion";
  DWORD type{};
  DWORD value{};
  DWORD bytes = sizeof(value);
  const DWORD status = get_value(
      hive,
      kVersionKey,
      value_name,
      &type,
      &value,
      &bytes);
  if (status != ERROR_SUCCESS || type != REG_DWORD ||
      bytes != sizeof(value)) {
    return clonecore::Result<std::uint32_t>::failure(make_error(
        status == ERROR_SUCCESS
            ? clonecore::ErrorCode::invalid_data
            : clonecore::ErrorCode::query_failed,
        status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status,
        L"オフラインWindowsバージョン照会",
        L"必須レジストリ数値を取得できません"));
  }
  return clonecore::Result<std::uint32_t>::success(value);
}

clonecore::Result<OfflineWindowsVersion>
read_offline_windows_version_with_offreg(
    const std::wstring& hive_path,
    const std::wstring& expected_dll_path) {
  UniqueLibrary library(LoadLibraryExW(
      L"offreg.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
  if (library.get() == nullptr) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"WindowsオフラインレジストリDLL読込み",
            GetLastError()));
  }
  const auto module_status =
      verify_loaded_module_path(library.get(), expected_dll_path);
  if (!module_status) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        module_status.error());
  }

  const auto open_hive = load_offline_registry_function<OrOpenHiveFunction>(
      library.get(), "OROpenHive");
  const auto get_value = load_offline_registry_function<OrGetValueFunction>(
      library.get(), "ORGetValue");
  const auto close_hive = load_offline_registry_function<OrCloseHiveFunction>(
      library.get(), "ORCloseHive");
  if (open_hive == nullptr || get_value == nullptr || close_hive == nullptr) {
    return clonecore::Result<OfflineWindowsVersion>::failure(make_error(
        clonecore::ErrorCode::query_failed,
        ERROR_PROC_NOT_FOUND,
        L"WindowsオフラインレジストリAPI検証",
        L"必須の読み取り専用APIがありません"));
  }

  OrHandle raw_hive{};
  const DWORD open_status = open_hive(hive_path.c_str(), &raw_hive);
  if (open_status != ERROR_SUCCESS || raw_hive == nullptr) {
    return clonecore::Result<OfflineWindowsVersion>::failure(make_error(
        clonecore::ErrorCode::query_failed,
        open_status == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : open_status,
        L"オフラインWindows SOFTWARE hive読取り",
        L"レジストリhiveを読み取り専用で検証できません"));
  }
  UniqueOfflineHive hive(raw_hive, close_hive);

  const auto major = query_offline_registry_dword(
      get_value, hive.get(), L"CurrentMajorVersionNumber");
  const auto build_text = query_offline_registry_text(
      get_value, hive.get(), L"CurrentBuildNumber");
  const auto installation_type = query_offline_registry_text(
      get_value, hive.get(), L"InstallationType");
  if (!major) {
    return clonecore::Result<OfflineWindowsVersion>::failure(major.error());
  }
  if (!build_text) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        build_text.error());
  }
  if (!installation_type) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        installation_type.error());
  }
  const auto build = parse_build_number(build_text.value());
  if (!build) {
    return clonecore::Result<OfflineWindowsVersion>::failure(build.error());
  }
  const DWORD close_status = hive.close();
  if (close_status != ERROR_SUCCESS) {
    return clonecore::Result<OfflineWindowsVersion>::failure(make_error(
        clonecore::ErrorCode::query_failed,
        close_status,
        L"オフラインWindows SOFTWARE hive解放",
        L"読み取り専用hiveを安全に解放できません"));
  }
  return clonecore::Result<OfflineWindowsVersion>::success(
      OfflineWindowsVersion{
          .major = major.value(),
          .build = build.value(),
          .installation_type = installation_type.value()});
}

clonecore::Result<std::uint16_t> read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint16_t)) {
    return clonecore::Result<std::uint16_t>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オフラインWindows PEヘッダー解析",
        L"16ビット値がPEヘッダー範囲外です"));
  }
  const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
  const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
  return clonecore::Result<std::uint16_t>::success(
      static_cast<std::uint16_t>(low | (high << 8U)));
}

clonecore::Result<std::uint32_t> read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
    return clonecore::Result<std::uint32_t>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オフラインWindows PEヘッダー解析",
        L"32ビット値がPEヘッダー範囲外です"));
  }
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return clonecore::Result<std::uint32_t>::success(value);
}

bool is_drive_root(const std::wstring_view value) {
  return value.size() == 3 && std::iswalpha(value[0]) != 0 &&
         value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

bool is_volume_guid_root(const std::wstring_view value) {
  constexpr std::wstring_view kPrefix = L"\\\\?\\Volume{";
  constexpr std::size_t kGuidCharacters = 36U;
  constexpr std::array<std::size_t, 4> kHyphens{8U, 13U, 18U, 23U};
  constexpr std::size_t kExpectedSize =
      kPrefix.size() + kGuidCharacters + 2U;
  if (value.size() != kExpectedSize || value.back() != L'\\' ||
      value[value.size() - 2U] != L'}') {
    return false;
  }
  for (std::size_t index = 0; index < kPrefix.size(); ++index) {
    if (std::towlower(value[index]) != std::towlower(kPrefix[index])) {
      return false;
    }
  }
  for (std::size_t index = 0; index < kGuidCharacters; ++index) {
    const bool hyphen = std::find(kHyphens.begin(), kHyphens.end(), index) !=
        kHyphens.end();
    const wchar_t value_character = value[kPrefix.size() + index];
    if ((hyphen && value_character != L'-') ||
        (!hyphen && std::iswxdigit(value_character) == 0)) {
      return false;
    }
  }
  return true;
}

}  // namespace

clonecore::Result<OfflineWindowsVersion>
read_offline_windows_version_hive(const std::wstring& hive_path) {
  if (hive_path.empty()) {
    return clonecore::Result<OfflineWindowsVersion>::failure(make_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オフラインWindows SOFTWARE hive検証",
        L"SOFTWARE hiveの絶対パスが必要です"));
  }
  const auto file_status = verify_regular_file(
      hive_path, L"オフラインWindows SOFTWARE hive検証");
  if (!file_status) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        file_status.error());
  }

  const auto dll_path = system32_file_path(L"offreg.dll");
  if (!dll_path) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        dll_path.error());
  }
  const DWORD dll_attributes = GetFileAttributesW(dll_path.value().c_str());
  if (dll_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      const auto values = read_windows_current_version_from_hive(hive_path);
      if (!values) {
        return clonecore::Result<OfflineWindowsVersion>::failure(
            values.error());
      }
      const auto build = parse_build_number(values.value().build);
      if (!build) {
        return clonecore::Result<OfflineWindowsVersion>::failure(
            build.error());
      }
      return clonecore::Result<OfflineWindowsVersion>::success(
          OfflineWindowsVersion{
              .major = values.value().major,
              .build = build.value(),
              .installation_type = values.value().installation_type});
    }
    return clonecore::Result<OfflineWindowsVersion>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"WindowsオフラインレジストリDLL属性取得",
            native_code));
  }
  const auto dll_status = verify_regular_file(
      dll_path.value(), L"WindowsオフラインレジストリDLL検証");
  if (!dll_status) {
    return clonecore::Result<OfflineWindowsVersion>::failure(
        dll_status.error());
  }
  return read_offline_windows_version_with_offreg(
      hive_path, dll_path.value());
}

clonecore::Result<PeArchitecture> inspect_pe_architecture(
    const std::span<const std::byte> image_prefix) {
  const auto dos_signature = read_u16(image_prefix, 0);
  if (!dos_signature) {
    return clonecore::Result<PeArchitecture>::failure(dos_signature.error());
  }
  if (dos_signature.value() != kDosSignature) {
    return clonecore::Result<PeArchitecture>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_EXE_FORMAT,
        L"オフラインWindows PEヘッダー解析",
        L"DOS MZシグネチャがありません"));
  }

  const auto pe_offset_value =
      read_u32(image_prefix, kDosPeOffsetField);
  if (!pe_offset_value) {
    return clonecore::Result<PeArchitecture>::failure(
        pe_offset_value.error());
  }
  const auto pe_offset =
      static_cast<std::size_t>(pe_offset_value.value());
  constexpr std::size_t kRequiredPeBytes =
      kOptionalHeaderOffset + sizeof(std::uint16_t);
  if (pe_offset > image_prefix.size() ||
      image_prefix.size() - pe_offset < kRequiredPeBytes) {
    return clonecore::Result<PeArchitecture>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_EXE_FORMAT,
        L"オフラインWindows PEヘッダー解析",
        L"PEヘッダー位置が検査可能な範囲外です"));
  }

  const auto pe_signature = read_u32(image_prefix, pe_offset);
  const auto machine =
      read_u16(image_prefix, pe_offset + kMachineOffset);
  const auto optional_header_size =
      read_u16(image_prefix, pe_offset + kOptionalHeaderSizeOffset);
  const auto optional_header_magic =
      read_u16(image_prefix, pe_offset + kOptionalHeaderOffset);
  if (!pe_signature || !machine || !optional_header_size ||
      !optional_header_magic) {
    return clonecore::Result<PeArchitecture>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_EXE_FORMAT,
        L"オフラインWindows PEヘッダー解析",
        L"COFFまたはオプショナルヘッダーが不完全です"));
  }
  if (pe_signature.value() != kPeSignature ||
      optional_header_size.value() < sizeof(std::uint16_t) ||
      static_cast<std::size_t>(optional_header_size.value()) >
          image_prefix.size() - pe_offset - kOptionalHeaderOffset) {
    return clonecore::Result<PeArchitecture>::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_EXE_FORMAT,
        L"オフラインWindows PEヘッダー解析",
        L"PEシグネチャまたはオプショナルヘッダー長が不正です"));
  }

  if (machine.value() == kMachineAmd64 &&
      optional_header_magic.value() == kOptionalHeaderPe32Plus) {
    return clonecore::Result<PeArchitecture>::success(
        PeArchitecture::amd64);
  }
  if (machine.value() == kMachineI386 &&
      optional_header_magic.value() == kOptionalHeaderPe32) {
    return clonecore::Result<PeArchitecture>::success(PeArchitecture::x86);
  }
  if (machine.value() == kMachineArm64 &&
      optional_header_magic.value() == kOptionalHeaderPe32Plus) {
    return clonecore::Result<PeArchitecture>::success(
        PeArchitecture::arm64);
  }
  return clonecore::Result<PeArchitecture>::success(PeArchitecture::unknown);
}

bool is_supported_offline_windows_version(
    const OfflineWindowsVersion& version) noexcept {
  return version.major == 10 &&
      version.build >= kWindows10InitialBuild &&
      version.installation_type == L"Client";
}

clonecore::Result<std::wstring> normalize_offline_windows_volume_root(
    const std::wstring_view volume_root) {
  if (is_drive_root(volume_root)) {
    std::wstring normalized(volume_root);
    normalized[0] = static_cast<wchar_t>(std::towupper(normalized[0]));
    normalized[2] = L'\\';
    return clonecore::Result<std::wstring>::success(std::move(normalized));
  }
  if (is_volume_guid_root(volume_root)) {
    return clonecore::Result<std::wstring>::success(
        std::wstring(volume_root));
  }
  return clonecore::Result<std::wstring>::failure(make_error(
      clonecore::ErrorCode::invalid_argument,
      ERROR_INVALID_PARAMETER,
      L"オフラインWindowsボリュームルート検証",
      L"ドライブまたは厳密なVolume GUIDのルートを指定してください"));
}

clonecore::Status verify_offline_windows_amd64(
    const std::wstring& windows_root) {
  const auto normalized = normalize_offline_windows_volume_root(windows_root);
  if (!normalized) {
    return clonecore::Status::failure(normalized.error());
  }
  const std::wstring& normalized_root = normalized.value();
  const std::wstring kernel_path =
      normalized_root + L"Windows\\System32\\ntoskrnl.exe";

  clonecore::UniqueHandle kernel(CreateFileW(
      kernel_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!kernel) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"オフラインWindowsカーネル読取り",
        GetLastError()));
  }

  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (!GetFileInformationByHandleEx(
          kernel.get(),
          FileAttributeTagInfo,
          &tag_info,
          sizeof(tag_info))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"オフラインWindowsカーネル属性取得",
        GetLastError()));
  }
  if ((tag_info.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"オフラインWindowsカーネル属性検証",
        L"ディレクトリまたはreparse pointのカーネルは使用できません"));
  }

  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(kernel.get(), &file_size)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"オフラインWindowsカーネルサイズ取得",
        GetLastError()));
  }
  if (file_size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(file_size.QuadPart) > kMaximumKernelBytes) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_INVALID,
        L"オフラインWindowsカーネルサイズ検証",
        L"カーネルのサイズが許可範囲外です"));
  }

  const auto prefix_size = static_cast<std::size_t>(
      (std::min)(
          static_cast<std::uint64_t>(file_size.QuadPart),
          static_cast<std::uint64_t>(kMaximumHeaderPrefixBytes)));
  if (prefix_size >
      static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::internal_error,
        ERROR_ARITHMETIC_OVERFLOW,
        L"オフラインWindowsカーネル読取り",
        L"読取りサイズをWindows APIへ安全に渡せません"));
  }
  std::vector<std::byte> prefix(prefix_size);
  DWORD bytes_read = 0;
  if (!ReadFile(
          kernel.get(),
          prefix.data(),
          static_cast<DWORD>(prefix.size()),
          &bytes_read,
          nullptr) ||
      bytes_read != prefix.size()) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"オフラインWindowsカーネルヘッダー読取り",
        GetLastError()));
  }

  const auto architecture = inspect_pe_architecture(prefix);
  if (!architecture) {
    return clonecore::Status::failure(architecture.error());
  }
  if (architecture.value() != PeArchitecture::amd64) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オフラインWindows x64検証",
        L"起動修復とPhase 4はWindows 10/11 x64（AMD64）だけを対象にします"));
  }
  const auto version = read_offline_windows_version_hive(
      normalized_root + L"Windows\\System32\\Config\\SOFTWARE");
  if (!version) {
    return clonecore::Status::failure(version.error());
  }
  if (!is_supported_offline_windows_version(version.value())) {
    return clonecore::Status::failure(make_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オフラインWindowsバージョン検証",
        L"起動修復とPhase 4はWindows 10/11クライアントだけを対象にします"));
  }
  return clonecore::success_status();
}

}  // namespace ytec::bootrepair

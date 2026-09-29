#include "ytec/migrationengine/windows_file_system_recreate.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>
#include <objbase.h>
#include <vds.h>
#include <winioctl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

constexpr std::uint32_t kVolumeArrivalAttempts = 120U;
constexpr ULONG kFileDirectoryFile = 0x00000001UL;
constexpr ULONG kFileWriteThrough = 0x00000002UL;
constexpr ULONG kFileNonDirectoryFile = 0x00000040UL;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020UL;
constexpr ULONG kFileOpenForBackupIntent = 0x00004000UL;
constexpr ULONG kFileOpenReparsePoint = 0x00200000UL;
constexpr ULONG kFileCreate = 0x00000002UL;
constexpr CLSID kVdsLoaderClassId{
    0x9C38ED61U,
    0xD565U,
    0x4728U,
    {0xAEU, 0xEEU, 0xC8U, 0x09U, 0x52U, 0xF0U, 0xECU, 0xDEU},
};

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE,
    ACCESS_MASK,
    POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK,
    PLARGE_INTEGER,
    ULONG,
    ULONG,
    ULONG,
    ULONG,
    PVOID,
    ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(NTSTATUS);

clonecore::Error target_error(
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
  return clonecore::Result<T>::failure(target_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(target_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool same_text_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size() ||
      left.size() > static_cast<std::size_t>(
          (std::numeric_limits<int>::max)())) {
    return false;
  }
  return left.empty() ||
      CompareStringOrdinal(
          left.data(),
          static_cast<int>(left.size()),
          right.data(),
          static_cast<int>(right.size()),
          TRUE) == CSTR_EQUAL;
}

struct OrdinalCaseInsensitiveLess final {
  bool operator()(
      const std::wstring& left,
      const std::wstring& right) const noexcept {
    if (left.empty() || right.empty() ||
        left.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)())) {
      return left < right;
    }
    const int compared = CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE);
    return compared == CSTR_LESS_THAN ||
        (compared == 0 && left < right);
  }
};

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
      (value >= L'a' && value <= L'f') ||
      (value >= L'A' && value <= L'F');
}

bool is_canonical_volume_guid_path(std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (!path.ends_with(L'\\') || !path.starts_with(prefix)) {
    return false;
  }
  path.remove_suffix(1U);
  if (!path.ends_with(L'}') || path.size() != prefix.size() + 37U) {
    return false;
  }
  const auto body = path.substr(prefix.size(), 36U);
  for (std::size_t index = 0U; index < body.size(); ++index) {
    const bool hyphen = index == 8U || index == 13U || index == 18U ||
        index == 23U;
    if ((hyphen && body[index] != L'-') ||
        (!hyphen && !is_hex(body[index]))) {
      return false;
    }
  }
  return true;
}

std::wstring trim_volume_slash(std::wstring path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
    path.pop_back();
  }
  return path;
}

DWORD native_status_error(const NTSTATUS status) noexcept {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return ERROR_GEN_FAILURE;
  }
  const auto converter = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
      GetProcAddress(ntdll, "RtlNtStatusToDosError"));
  return converter == nullptr
      ? ERROR_GEN_FAILURE
      : static_cast<DWORD>(converter(status));
}

clonecore::Result<NtCreateFileFunction> nt_create_file_function() {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return clonecore::Result<NtCreateFileFunction>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::internal_error,
            L"再作成target NtCreateFile取得",
            GetLastError()));
  }
  const auto function = reinterpret_cast<NtCreateFileFunction>(
      GetProcAddress(ntdll, "NtCreateFile"));
  if (function == nullptr) {
    return clonecore::Result<NtCreateFileFunction>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::unsupported_platform,
            L"再作成target NtCreateFile取得",
            ERROR_PROC_NOT_FOUND));
  }
  return clonecore::Result<NtCreateFileFunction>::success(function);
}

struct ExactVolumeExtent final {
  std::uint32_t disk_number{};
  std::uint64_t offset{};
  std::uint64_t length{};
};

clonecore::Result<ExactVolumeExtent> query_exact_volume_extent(
    const HANDLE volume) {
  std::vector<std::byte> buffer(64U * 1024U);
  DWORD returned = 0U;
  if (DeviceIoControl(
          volume,
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0U,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &returned,
          nullptr) == FALSE) {
    return clonecore::Result<ExactVolumeExtent>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"再作成target Volume extent照会",
            GetLastError()));
  }
  constexpr std::size_t header = offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (returned < header + sizeof(DISK_EXTENT)) {
    return failure<ExactVolumeExtent>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"再作成target Volume extent応答",
        L"Volume extent応答が固定headerより短いです");
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents != 1U ||
      extents->Extents[0].StartingOffset.QuadPart < 0 ||
      extents->Extents[0].ExtentLength.QuadPart <= 0) {
    return failure<ExactVolumeExtent>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成target Volume extent一意性",
        L"Volumeを単一の正のphysical extentへ拘束できません");
  }
  return clonecore::Result<ExactVolumeExtent>::success({
      .disk_number = extents->Extents[0].DiskNumber,
      .offset = static_cast<std::uint64_t>(
          extents->Extents[0].StartingOffset.QuadPart),
      .length = static_cast<std::uint64_t>(
          extents->Extents[0].ExtentLength.QuadPart),
  });
}

clonecore::Result<ExactVolumeExtent> query_exact_volume_extent(
    const std::wstring& volume_guid_path) {
  clonecore::UniqueHandle volume(CreateFileW(
      trim_volume_slash(volume_guid_path).c_str(),
      0U,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return clonecore::Result<ExactVolumeExtent>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"再作成target Volume extent open",
            GetLastError()));
  }
  return query_exact_volume_extent(volume.get());
}

clonecore::Status validate_exact_binding(
    const WindowsFileSystemRecreateConstructionVolumeBinding& binding) {
  if (!is_canonical_volume_guid_path(binding.canonical_volume_guid_path) ||
      !binding.exact_single_disk_extent) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        L"再作成target Volume GUID binding",
        L"canonical Volume GUIDまたはsingle-extent証跡がありません");
  }
  auto extent = query_exact_volume_extent(binding.canonical_volume_guid_path);
  if (!extent) {
    return clonecore::Status::failure(extent.error());
  }
  if (extent.value().disk_number != binding.disk_number ||
      extent.value().offset != binding.target_offset ||
      extent.value().length != binding.target_size) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"再作成target Volume extent再照合",
        L"Volumeのdisk、offset、またはlengthがレビュー済み範囲と一致しません");
  }
  return clonecore::success_status();
}

struct OpenedHandleIdentity final {
  std::array<std::byte, 16U> file_id{};
  std::uint64_t volume_serial_number{};
  std::uint64_t size_bytes{};
  std::uint64_t creation_time{};
  std::uint64_t last_write_time{};
  std::uint32_t attributes{};
  std::uint32_t reparse_tag{};
  std::uint32_t links{};
  bool directory{};
};

clonecore::Result<OpenedHandleIdentity> inspect_opened_handle(
    const HANDLE handle,
    const bool expected_directory,
    const std::wstring_view operation) {
  FILE_ID_INFO id{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileInformationByHandleEx(
          handle, FileIdInfo, &id, sizeof(id)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE) {
    return clonecore::Result<OpenedHandleIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  const bool directory = standard.Directory != FALSE;
  if (standard.DeletePending != FALSE || directory != expected_directory ||
      standard.EndOfFile.QuadPart < 0 || standard.NumberOfLinks != 1U ||
      tag.ReparseTag != 0U ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      basic.CreationTime.QuadPart < 0 || basic.LastWriteTime.QuadPart < 0) {
    return failure<OpenedHandleIdentity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        std::wstring(operation),
        L"opened handleを単一link・non-reparseの通常file/directoryとして証明できません");
  }
  OpenedHandleIdentity result{
      .volume_serial_number = id.VolumeSerialNumber,
      .size_bytes = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .creation_time = static_cast<std::uint64_t>(
          basic.CreationTime.QuadPart),
      .last_write_time = static_cast<std::uint64_t>(
          basic.LastWriteTime.QuadPart),
      .attributes = tag.FileAttributes,
      .reparse_tag = tag.ReparseTag,
      .links = standard.NumberOfLinks,
      .directory = directory,
  };
  static_assert(sizeof(result.file_id) == sizeof(id.FileId.Identifier));
  std::memcpy(
      result.file_id.data(), id.FileId.Identifier, result.file_id.size());
  return clonecore::Result<OpenedHandleIdentity>::success(result);
}

bool same_opened_object(
    const OpenedHandleIdentity& left,
    const OpenedHandleIdentity& right) noexcept {
  return left.file_id == right.file_id &&
      left.volume_serial_number == right.volume_serial_number &&
      left.directory == right.directory && left.reparse_tag == 0U &&
      right.reparse_tag == 0U && left.links == 1U && right.links == 1U;
}

std::uint32_t portable_attributes(const std::uint32_t attributes) noexcept {
  std::uint32_t result{};
  if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
    result |= migrationcore::recreate_attribute_read_only;
  }
  if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0U) {
    result |= migrationcore::recreate_attribute_hidden;
  }
  if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0U) {
    result |= migrationcore::recreate_attribute_system;
  }
  if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0U) {
    result |= migrationcore::recreate_attribute_archive;
  }
  return result;
}

DWORD windows_attributes(const std::uint32_t portable) noexcept {
  DWORD result{};
  if ((portable & migrationcore::recreate_attribute_read_only) != 0U) {
    result |= FILE_ATTRIBUTE_READONLY;
  }
  if ((portable & migrationcore::recreate_attribute_hidden) != 0U) {
    result |= FILE_ATTRIBUTE_HIDDEN;
  }
  if ((portable & migrationcore::recreate_attribute_system) != 0U) {
    result |= FILE_ATTRIBUTE_SYSTEM;
  }
  if ((portable & migrationcore::recreate_attribute_archive) != 0U) {
    result |= FILE_ATTRIBUTE_ARCHIVE;
  }
  return result == 0U ? FILE_ATTRIBUTE_NORMAL : result;
}

clonecore::Status apply_portable_metadata(
    const HANDLE handle,
    const migrationcore::CanonicalFileSystemTreeEntry& entry,
    const std::wstring_view operation,
    const bool flush_file_handle) {
  if (entry.creation_time_utc_100ns > static_cast<std::uint64_t>(
          (std::numeric_limits<LONGLONG>::max)()) ||
      entry.last_write_time_utc_100ns > static_cast<std::uint64_t>(
          (std::numeric_limits<LONGLONG>::max)())) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"portable timestampをFILE_BASIC_INFOで表現できません");
  }
  FILE_BASIC_INFO basic{};
  basic.CreationTime.QuadPart =
      static_cast<LONGLONG>(entry.creation_time_utc_100ns);
  basic.LastWriteTime.QuadPart =
      static_cast<LONGLONG>(entry.last_write_time_utc_100ns);
  basic.FileAttributes = windows_attributes(entry.portable_attributes);
  if (SetFileInformationByHandle(
          handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
      (flush_file_handle && FlushFileBuffers(handle) == FALSE)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed, operation, GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<clonecore::UniqueHandle> create_relative_no_replace(
    const HANDLE parent,
    const std::wstring_view component,
    const bool directory) {
  if (parent == nullptr || parent == INVALID_HANDLE_VALUE ||
      component.empty() ||
      component.size() >
          migrationcore::kMaximumFileSystemRecreateComponentUtf16Units ||
      component.size() >
          (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t) ||
      component.find_first_of(L"\\/") != component.npos) {
    return failure<clonecore::UniqueHandle>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"再作成target handle-relative no-replace",
        L"親handleまたは相対componentが固定上限外です");
  }
  auto native = nt_create_file_function();
  if (!native) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(native.error());
  }
  UNICODE_STRING relative{};
  relative.Buffer = const_cast<PWSTR>(component.data());
  relative.Length = static_cast<USHORT>(component.size() * sizeof(wchar_t));
  relative.MaximumLength = relative.Length;
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(
      &attributes,
      &relative,
      OBJ_CASE_INSENSITIVE,
      parent,
      nullptr);
  IO_STATUS_BLOCK io{};
  HANDLE opened = INVALID_HANDLE_VALUE;
  const ACCESS_MASK access = FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
      SYNCHRONIZE |
      (directory
           ? FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY
           : FILE_READ_DATA | FILE_WRITE_DATA);
  const ULONG options = kFileOpenReparsePoint |
      kFileOpenForBackupIntent | kFileSynchronousIoNonAlert |
      kFileWriteThrough |
      (directory ? kFileDirectoryFile : kFileNonDirectoryFile);
  const NTSTATUS status = native.value()(
      &opened,
      access,
      &attributes,
      &io,
      nullptr,
      directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ,
      kFileCreate,
      options,
      nullptr,
      0U);
  if (status < 0) {
    const DWORD native_code = native_status_error(status);
    if (opened != nullptr && opened != INVALID_HANDLE_VALUE) {
      CloseHandle(opened);
    }
    return failure<clonecore::UniqueHandle>(
        native_code == ERROR_ACCESS_DENIED
            ? clonecore::ErrorCode::access_denied
            : clonecore::ErrorCode::io_failed,
        native_code,
        L"再作成target handle-relative no-replace",
        L"RootDirectory相対・reparse非追跡・FILE_CREATEでobjectを作成できませんでした");
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      clonecore::UniqueHandle(opened));
}

std::wstring parent_path(const std::wstring& path) {
  const std::size_t separator = path.rfind(L'\\');
  return separator == std::wstring::npos
      ? std::wstring{}
      : path.substr(0U, separator);
}

std::wstring_view leaf_name(const std::wstring& path) noexcept {
  const std::size_t separator = path.rfind(L'\\');
  return separator == std::wstring::npos
      ? std::wstring_view(path)
      : std::wstring_view(path).substr(separator + 1U);
}

class ScopedComInitialization final {
 public:
  explicit ScopedComInitialization(const bool uninitialize) noexcept
      : uninitialize_(uninitialize) {}
  ~ScopedComInitialization() {
    if (uninitialize_) {
      CoUninitialize();
    }
  }
  ScopedComInitialization(const ScopedComInitialization&) = delete;
  ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

 private:
  bool uninitialize_{};
};

template <typename Interface>
class UniqueComInterface final {
 public:
  UniqueComInterface() noexcept = default;
  explicit UniqueComInterface(Interface* value) noexcept : value_(value) {}
  ~UniqueComInterface() { reset(); }
  UniqueComInterface(const UniqueComInterface&) = delete;
  UniqueComInterface& operator=(const UniqueComInterface&) = delete;
  UniqueComInterface(UniqueComInterface&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  UniqueComInterface& operator=(UniqueComInterface&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] Interface* get() const noexcept { return value_; }
  [[nodiscard]] Interface** put() noexcept {
    reset();
    return &value_;
  }
  Interface* operator->() const noexcept { return value_; }
  explicit operator bool() const noexcept { return value_ != nullptr; }

 private:
  void reset() noexcept {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }
  Interface* value_{};
};

clonecore::Status format_exact_volume_with_vds(
    const WindowsFileSystemRecreateConstructionVolumeBinding& binding,
    const migrationcore::FileSystemRecreateFormatGeometry& desired) {
  auto exact = validate_exact_binding(binding);
  if (!exact) {
    return exact;
  }
  if ((desired.file_system != migrationcore::MigrationFileSystem::fat32 &&
       desired.file_system != migrationcore::MigrationFileSystem::exfat) ||
      desired.target_volume_bytes != binding.target_size ||
      desired.cluster_size < 512U ||
      desired.cluster_size > 32ULL * 1024ULL * 1024ULL ||
      (desired.cluster_size & (desired.cluster_size - 1U)) != 0U ||
      desired.cluster_size > (std::numeric_limits<ULONG>::max)()) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成target VDS固定format引数",
        L"FAT32/exFAT、exact volume、またはallocation unitが対応条件外です");
  }

  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool must_uninitialize = initialized == S_OK || initialized == S_FALSE;
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return status_failure(
        clonecore::ErrorCode::unsupported_platform,
        static_cast<DWORD>(initialized),
        L"再作成target VDS COM初期化",
        L"Microsoft VDS formatterのCOM境界を初期化できません");
  }
  const ScopedComInitialization com_scope(must_uninitialize);
  UniqueComInterface<IVdsServiceLoader> loader;
  HRESULT result = CoCreateInstance(
      kVdsLoaderClassId,
      nullptr,
      CLSCTX_LOCAL_SERVER,
      IID_PPV_ARGS(loader.put()));
  if (result != S_OK || !loader) {
    return status_failure(
        clonecore::ErrorCode::unsupported_platform,
        static_cast<DWORD>(result),
        L"再作成target VDS loader取得",
        L"Microsoft VDS loaderを一意に取得できません");
  }
  UniqueComInterface<IVdsService> service;
  result = loader->LoadService(nullptr, service.put());
  if (result != S_OK || !service) {
    return status_failure(
        clonecore::ErrorCode::unsupported_platform,
        static_cast<DWORD>(result),
        L"再作成target VDS service取得",
        L"Microsoft VDS serviceを読み込めません");
  }
  result = service->WaitForServiceReady();
  if (result == S_OK) {
    result = service->Reenumerate();
  }
  if (result == S_OK) {
    result = service->Refresh();
  }
  if (result != S_OK) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"再作成target VDS service再列挙",
        L"VDS serviceの準備・再列挙・refreshの全てを確認できません");
  }

  UniqueComInterface<IEnumVdsObject> providers;
  result = service->QueryProviders(
      VDS_QUERY_SOFTWARE_PROVIDERS, providers.put());
  if (result != S_OK || !providers) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"再作成target VDS provider列挙",
        L"Microsoft software providerを列挙できません");
  }
  const std::wstring expected_name =
      trim_volume_slash(binding.canonical_volume_guid_path);
  UniqueComInterface<IVdsVolumeMF2> formatter;
  while (true) {
    IUnknown* provider_raw = nullptr;
    ULONG provider_count = 0U;
    result = providers->Next(1U, &provider_raw, &provider_count);
    if (result == S_FALSE && provider_count == 0U) {
      break;
    }
    if (result != S_OK || provider_count != 1U || provider_raw == nullptr) {
      return status_failure(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"再作成target VDS provider取得",
          L"VDS provider列挙結果が一意ではありません");
    }
    UniqueComInterface<IUnknown> provider_unknown(provider_raw);
    UniqueComInterface<IVdsSwProvider> provider;
    result = provider_unknown->QueryInterface(IID_PPV_ARGS(provider.put()));
    if (result != S_OK || !provider) {
      return status_failure(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"再作成target VDS provider照合",
          L"列挙objectをsoftware providerとして照合できません");
    }
    UniqueComInterface<IEnumVdsObject> packs;
    result = provider->QueryPacks(packs.put());
    if (result != S_OK || !packs) {
      return status_failure(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"再作成target VDS pack列挙",
          L"software providerのpackを列挙できません");
    }
    while (true) {
      IUnknown* pack_raw = nullptr;
      ULONG pack_count = 0U;
      result = packs->Next(1U, &pack_raw, &pack_count);
      if (result == S_FALSE && pack_count == 0U) {
        break;
      }
      if (result != S_OK || pack_count != 1U || pack_raw == nullptr) {
        return status_failure(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"再作成target VDS pack取得",
            L"VDS pack列挙結果が一意ではありません");
      }
      UniqueComInterface<IUnknown> pack_unknown(pack_raw);
      UniqueComInterface<IVdsPack> pack;
      result = pack_unknown->QueryInterface(IID_PPV_ARGS(pack.put()));
      if (result != S_OK || !pack) {
        return status_failure(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"再作成target VDS pack照合",
            L"列挙objectをsoftware packとして照合できません");
      }
      UniqueComInterface<IEnumVdsObject> volumes;
      result = pack->QueryVolumes(volumes.put());
      if (result != S_OK || !volumes) {
        return status_failure(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"再作成target VDS Volume列挙",
            L"software packのVolumeを列挙できません");
      }
      while (true) {
        IUnknown* volume_raw = nullptr;
        ULONG volume_count = 0U;
        result = volumes->Next(1U, &volume_raw, &volume_count);
        if (result == S_FALSE && volume_count == 0U) {
          break;
        }
        if (result != S_OK || volume_count != 1U || volume_raw == nullptr) {
          return status_failure(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"再作成target VDS Volume取得",
              L"VDS Volume列挙結果が一意ではありません");
        }
        UniqueComInterface<IUnknown> volume_unknown(volume_raw);
        UniqueComInterface<IVdsVolume> volume;
        result = volume_unknown->QueryInterface(IID_PPV_ARGS(volume.put()));
        if (result != S_OK || !volume) {
          return status_failure(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"再作成target VDS Volume照合",
              L"列挙objectをVDS Volumeとして照合できません");
        }
        VDS_VOLUME_PROP properties{};
        result = volume->GetProperties(&properties);
        std::wstring name;
        if (result == S_OK && properties.pwszName != nullptr) {
          name = trim_volume_slash(properties.pwszName);
        }
        CoTaskMemFree(properties.pwszName);
        if (result != S_OK) {
          return status_failure(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"再作成target VDS Volume属性",
              L"VDS Volume属性を取得できません");
        }
        if (!same_text_case_insensitive(name, expected_name)) {
          continue;
        }
        constexpr ULONG forbidden_flags =
            VDS_VF_SYSTEM_VOLUME | VDS_VF_BOOT_VOLUME | VDS_VF_PAGEFILE |
            VDS_VF_HIBERNATION | VDS_VF_CRASHDUMP |
            VDS_VF_NOT_FORMATTABLE | VDS_VF_SHADOW_COPY |
            VDS_VF_FVE_ENABLED | VDS_VF_BACKS_BOOT_VOLUME |
            VDS_VF_BACKED_BY_WIM_IMAGE;
        const bool failed_health = properties.health == VDS_H_FAILING ||
            properties.health == VDS_H_PENDING_FAILURE ||
            properties.health == VDS_H_FAILED;
        if (formatter || properties.type != VDS_VT_SIMPLE ||
            properties.status != VDS_VS_ONLINE ||
            properties.TransitionState != VDS_TS_STABLE ||
            properties.ullSize != binding.target_size ||
            (properties.ulFlags & forbidden_flags) != 0U || failed_health) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"再作成target VDS exact Volume拘束",
              L"VDS Volumeの一意性、online/stable状態、寸法、属性、またはhealthが一致しません");
        }
        result = volume->QueryInterface(IID_PPV_ARGS(formatter.put()));
        if (result != S_OK || !formatter) {
          return status_failure(
              clonecore::ErrorCode::unsupported_platform,
              static_cast<DWORD>(result),
              L"再作成target VDS FormatEx取得",
              L"対象VolumeにMicrosoft VDS FormatExがありません");
        }
      }
    }
  }
  if (!formatter) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"再作成target VDS exact Volume検索",
        L"canonical Volume GUIDに一致する安全なVDS Volumeが1件ありません");
  }
  exact = validate_exact_binding(binding);
  if (!exact) {
    return exact;
  }
  std::wstring file_system_name =
      desired.file_system == migrationcore::MigrationFileSystem::fat32
      ? L"FAT32"
      : L"EXFAT";
  std::wstring empty_label;
  UniqueComInterface<IVdsAsync> asynchronous;
  result = formatter->FormatEx(
      file_system_name.data(),
      0U,
      static_cast<ULONG>(desired.cluster_size),
      empty_label.data(),
      TRUE,
      TRUE,
      FALSE,
      asynchronous.put());
  if (result != S_OK || !asynchronous) {
    return status_failure(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(result),
        L"再作成target VDS FormatEx開始",
        L"固定filesystem、空label、固定allocation unitでquick formatを開始できません");
  }
  HRESULT operation_result = E_FAIL;
  VDS_ASYNC_OUTPUT output{};
  result = asynchronous->Wait(&operation_result, &output);
  if (result != S_OK || operation_result != S_OK ||
      output.type != VDS_ASYNCOUT_FORMAT) {
    return status_failure(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(result != S_OK ? result : operation_result),
        L"再作成target VDS FormatEx完了",
        L"VDS formatの戻り値をS_OK/FORMATとして確認できません。再起動要求を含む非S_OKは受理しません");
  }
  return validate_exact_binding(binding);
}

struct FileState final {
  clonecore::UniqueHandle handle;
  OpenedHandleIdentity created_identity;
  migrationcore::CanonicalFileSystemTreeEntry expected;
  std::uint64_t written_bytes{};
  bool finalized{};
};

class Win32FileSystemRecreateTargetIo final
    : public IWindowsFileSystemRecreateTargetIo {
 public:
  Win32FileSystemRecreateTargetIo(
      clonecore::StableDiskIdentity expected_target,
      clonecore::TargetConfirmation confirmation,
      const bool active_rescue_media,
      clonecore::DiskOperationCallbacks callbacks)
      : expected_target_(std::move(expected_target)),
        confirmation_(std::move(confirmation)),
        active_rescue_media_(active_rescue_media),
        callbacks_(std::move(callbacks)) {}

  ~Win32FileSystemRecreateTargetIo() override {
    close_namespace_handles();
  }

  [[nodiscard]] clonecore::Result<
      WindowsFileSystemRecreateTargetDiskObservation>
  observe_target_read_only() override {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    if (!inventory) {
      return failure<WindowsFileSystemRecreateTargetDiskObservation>(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"再作成target inventory生成",
          L"Windows disk inventory providerを生成できません");
    }
    auto observed = diskmodel::reidentify_physical_target(
        expected_target_, confirmation_, *inventory);
    if (!observed) {
      return clonecore::Result<
          WindowsFileSystemRecreateTargetDiskObservation>::failure(
          observed.error());
    }
    const auto target_class =
        imageformat::classify_tsumugi_physical_restore_target(
            observed.value().target);
    const auto accepted =
        imageformat::validate_tsumugi_physical_restore_target(
            observed.value().target,
            target_class,
            active_rescue_media_);
    if (!accepted) {
      return clonecore::Result<
          WindowsFileSystemRecreateTargetDiskObservation>::failure(
          accepted.error());
    }
    auto layout = imageformat::hash_tsumugi_physical_restore_target_layout_v1(
        observed.value().target);
    if (!layout) {
      return clonecore::Result<
          WindowsFileSystemRecreateTargetDiskObservation>::failure(
          layout.error());
    }
    return clonecore::Result<
        WindowsFileSystemRecreateTargetDiskObservation>::success({
        .physical = observed.take_value(),
        .current_layout_sha256 = layout.take_value(),
        .target_class_accepted = true,
    });
  }

  [[nodiscard]] clonecore::Status set_target_offline(
      const bool offline) override {
    return diskmodel::set_verified_physical_target_offline_with_windows_apis(
        expected_target_, confirmation_, offline);
  }

  [[nodiscard]] clonecore::Result<diskmodel::PhysicalTargetHandle>
  open_offline_target() override {
    return diskmodel::open_verified_physical_target_with_windows_apis(
        expected_target_, confirmation_);
  }

  [[nodiscard]] clonecore::Status notify_layout_changed() override {
    auto observed = observe_target_read_only();
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    clonecore::UniqueHandle disk(CreateFileW(
        observed.value().physical.target.device_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!disk) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"再作成target layout通知open",
          GetLastError()));
    }
    STORAGE_DEVICE_NUMBER number{};
    DWORD returned = 0U;
    if (DeviceIoControl(
            disk.get(),
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0U,
            &number,
            sizeof(number),
            &returned,
            nullptr) == FALSE ||
        returned < sizeof(number) || number.DeviceType != FILE_DEVICE_DISK ||
        number.DeviceNumber !=
            observed.value().physical.target.disk_number) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"再作成target layout通知handle拘束",
          L"fresh reidentifyしたPhysicalDriveとopened handleが一致しません");
    }
    if (DeviceIoControl(
            disk.get(),
            IOCTL_DISK_UPDATE_PROPERTIES,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"再作成target layout更新通知",
          GetLastError()));
    }
    auto verified = observe_target_read_only();
    return verified
        ? clonecore::success_status()
        : clonecore::Status::failure(verified.error());
  }

  [[nodiscard]] clonecore::Result<
      WindowsFileSystemRecreateConstructionVolumeBinding>
  bind_online_construction_volume(
      const std::uint32_t final_target_number,
      const std::uint64_t target_offset,
      const std::uint64_t target_size) override {
    if (final_target_number == 0U) {
      return failure<WindowsFileSystemRecreateConstructionVolumeBinding>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"再作成target construction Volume番号",
          L"final target numberが0です");
    }
    std::optional<clonecore::Error> last_error;
    for (std::uint32_t attempt = 0U;
         attempt < kVolumeArrivalAttempts;
         ++attempt) {
      auto observed = observe_target_read_only();
      if (!observed) {
        return clonecore::Result<
            WindowsFileSystemRecreateConstructionVolumeBinding>::failure(
            observed.error());
      }
      if (observed.value().physical.target.offline.value_or(true)) {
        return failure<WindowsFileSystemRecreateConstructionVolumeBinding>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_STATE,
            L"再作成target construction online確認",
            L"仮GPT公開後のphysical targetがonlineではありません");
      }
      const std::array<diskmodel::VolumePartitionLocation, 1U> expected{{
          {
              .table_index = final_target_number - 1U,
              .offset_bytes = target_offset,
          },
      }};
      auto volumes = diskmodel::query_windows_volume_bindings_by_offset(
          observed.value().physical.target, expected);
      if (volumes && volumes.value().size() == 1U) {
        WindowsFileSystemRecreateConstructionVolumeBinding binding{
            .final_target_number = final_target_number,
            .disk_number = observed.value().physical.target.disk_number,
            .target_offset = target_offset,
            .target_size = target_size,
            .canonical_volume_guid_path =
                volumes.value().front().volume_device_path,
            .exact_single_disk_extent = true,
        };
        if (!binding.canonical_volume_guid_path.ends_with(L'\\')) {
          binding.canonical_volume_guid_path.push_back(L'\\');
        }
        const auto exact = validate_exact_binding(binding);
        if (exact) {
          return clonecore::Result<
              WindowsFileSystemRecreateConstructionVolumeBinding>::success(
              std::move(binding));
        }
        last_error = exact.error();
      } else if (!volumes) {
        last_error = volumes.error();
      }
      if (clonecore::disk_operation_cancellation_requested(callbacks_)) {
        return failure<WindowsFileSystemRecreateConstructionVolumeBinding>(
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED,
            L"再作成target construction Volume待機",
            L"Volume到着待ちを安全な境界で取り消しました");
      }
      Sleep(250U);
    }
    return clonecore::Result<
        WindowsFileSystemRecreateConstructionVolumeBinding>::failure(
        last_error.value_or(target_error(
            clonecore::ErrorCode::query_failed,
            ERROR_TIMEOUT,
            L"再作成target construction Volume待機",
            L"exact Volume GUIDを30秒以内に一意に拘束できません")));
  }

  [[nodiscard]] clonecore::Status format_exact_volume(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume,
      const migrationcore::FileSystemRecreateFormatGeometry& desired)
      override {
    auto observed = observe_target_read_only();
    if (!observed || observed.value().physical.target.offline.value_or(true)) {
      return observed
          ? status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_STATE,
                L"再作成target format直前online確認",
                L"fresh targetがonlineではありません")
          : clonecore::Status::failure(observed.error());
    }
    if (clonecore::disk_operation_cancellation_requested(callbacks_)) {
      return status_failure(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"再作成target VDS format開始前取消",
          L"format開始前の安全な境界で取り消しました");
    }
    auto formatted = format_exact_volume_with_vds(volume, desired);
    if (!formatted) {
      return formatted;
    }
    observed = observe_target_read_only();
    if (!observed || observed.value().physical.target.offline.value_or(true)) {
      return observed
          ? status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_STATE,
                L"再作成target format後online確認",
                L"VDS完了後のfresh targetがonlineではありません")
          : clonecore::Status::failure(observed.error());
    }
    return validate_exact_binding(volume);
  }

  [[nodiscard]] clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  open_and_inspect_formatted_root(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume,
      const migrationcore::FileSystemRecreateFormatGeometry& desired)
      override {
    close_namespace_handles();
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return clonecore::Result<
          WindowsFileSystemRecreateFormattedRootObservation>::failure(
          exact.error());
    }
    clonecore::UniqueHandle root(CreateFileW(
        trim_volume_slash(volume.canonical_volume_guid_path).c_str(),
        FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!root) {
      return clonecore::Result<
          WindowsFileSystemRecreateFormattedRootObservation>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::access_denied,
              L"再作成target formatted root exclusive open",
              GetLastError()));
    }
    auto identity = inspect_opened_handle(
        root.get(), true, L"再作成target formatted root FileId");
    if (!identity) {
      return clonecore::Result<
          WindowsFileSystemRecreateFormattedRootObservation>::failure(
          identity.error());
    }
    root_ = std::move(root);
    root_identity_ = identity.take_value();
    binding_ = volume;
    geometry_ = desired;
    auto observation = inspect_retained_root();
    if (!observation) {
      close_namespace_handles();
    }
    return observation;
  }

  [[nodiscard]] clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  revalidate_formatted_root_read_only() override {
    return inspect_retained_root();
  }

  [[nodiscard]] clonecore::Status create_directory_no_replace(
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto root = inspect_retained_root();
    if (!root || entry.kind !=
            migrationcore::FileSystemRecreateEntryKind::directory ||
        directories_.contains(entry.relative_path)) {
      return root
          ? status_failure(
                clonecore::ErrorCode::invalid_argument,
                ERROR_ALREADY_EXISTS,
                L"再作成target directory no-replace状態",
                L"entry種類または既存retained directory tokenが不正です")
          : clonecore::Status::failure(root.error());
    }
    const auto parent = parent_handle(entry.relative_path);
    if (!parent) {
      return clonecore::Status::failure(parent.error());
    }
    auto created = create_relative_no_replace(
        parent.value(), leaf_name(entry.relative_path), true);
    if (!created) {
      return clonecore::Status::failure(created.error());
    }
    auto identity = inspect_opened_handle(
        created.value().get(), true, L"再作成target created directory FileId");
    if (!identity || identity.value().size_bytes != 0U) {
      return identity
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"再作成target created directory初期状態",
                L"新規directoryのopened-handle状態が空・non-reparseではありません")
          : clonecore::Status::failure(identity.error());
    }
    directories_.emplace(entry.relative_path, created.take_value());
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<std::uint64_t> create_file_no_replace(
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto root = inspect_retained_root();
    if (!root || entry.kind !=
            migrationcore::FileSystemRecreateEntryKind::regular_file ||
        next_file_token_ == 0U ||
        next_file_token_ == (std::numeric_limits<std::uint64_t>::max)()) {
      return !root
          ? clonecore::Result<std::uint64_t>::failure(root.error())
          : failure<std::uint64_t>(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_STATE,
                L"再作成target file no-replace状態",
                L"entry種類またはopened handle token範囲が不正です");
    }
    const auto parent = parent_handle(entry.relative_path);
    if (!parent) {
      return clonecore::Result<std::uint64_t>::failure(parent.error());
    }
    auto created = create_relative_no_replace(
        parent.value(), leaf_name(entry.relative_path), false);
    if (!created) {
      return clonecore::Result<std::uint64_t>::failure(created.error());
    }
    auto identity = inspect_opened_handle(
        created.value().get(), false, L"再作成target created file FileId");
    if (!identity || identity.value().size_bytes != 0U) {
      return identity
          ? failure<std::uint64_t>(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"再作成target created file初期状態",
                L"新規fileのopened-handle状態が空・single-link・non-reparseではありません")
          : clonecore::Result<std::uint64_t>::failure(identity.error());
    }
    const std::uint64_t token = next_file_token_++;
    files_.emplace(token, FileState{
        .handle = created.take_value(),
        .created_identity = identity.take_value(),
        .expected = entry,
    });
    return clonecore::Result<std::uint64_t>::success(token);
  }

  [[nodiscard]] clonecore::Result<std::size_t> write_file_chunk(
      const std::uint64_t opened_handle_token,
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    auto root = inspect_retained_root();
    const auto file = files_.find(opened_handle_token);
    if (!root || file == files_.end() || file->second.finalized ||
        bytes.empty() ||
        bytes.size() > kDefaultWindowsFileSystemRecreateTransferBytes ||
        offset != file->second.written_bytes ||
        offset > file->second.expected.size_bytes ||
        bytes.size() > file->second.expected.size_bytes - offset ||
        offset > static_cast<std::uint64_t>(
            (std::numeric_limits<LONGLONG>::max)()) ||
        bytes.size() > (std::numeric_limits<DWORD>::max)()) {
      return !root
          ? clonecore::Result<std::size_t>::failure(root.error())
          : failure<std::size_t>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_STATE,
                L"再作成target opened-handle write境界",
                L"token、sequential offset、chunk、または固定file sizeが一致しません");
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD written = 0U;
    if (SetFilePointerEx(
            file->second.handle.get(), position, nullptr, FILE_BEGIN) == FALSE ||
        WriteFile(
            file->second.handle.get(),
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &written,
            nullptr) == FALSE ||
        written != bytes.size()) {
      const DWORD native = GetLastError();
      return failure<std::size_t>(
          clonecore::ErrorCode::io_failed,
          native == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native,
          L"再作成target opened-handle write",
          L"固定chunk全体をretained handleへ書き込めません");
    }
    file->second.written_bytes += written;
    return clonecore::Result<std::size_t>::success(written);
  }

  [[nodiscard]] clonecore::Status
  finalize_file_metadata_flush_and_hold(
      const std::uint64_t opened_handle_token,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto root = inspect_retained_root();
    const auto file = files_.find(opened_handle_token);
    if (!root || file == files_.end() || file->second.finalized ||
        file->second.expected.relative_path != entry.relative_path ||
        file->second.written_bytes != entry.size_bytes ||
        entry.size_bytes > static_cast<std::uint64_t>(
            (std::numeric_limits<LONGLONG>::max)())) {
      return !root
          ? clonecore::Status::failure(root.error())
          : status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_STATE,
                L"再作成target file finalize境界",
                L"token、entry、またはexact written lengthが一致しません");
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(entry.size_bytes);
    if (SetFilePointerEx(
            file->second.handle.get(), end, nullptr, FILE_BEGIN) == FALSE ||
        SetEndOfFile(file->second.handle.get()) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"再作成target exact EOF確定",
          GetLastError()));
    }
    auto status = apply_portable_metadata(
        file->second.handle.get(),
        entry,
        L"再作成target file metadata/flush",
        true);
    if (!status) {
      return status;
    }
    auto after = inspect_opened_handle(
        file->second.handle.get(), false, L"再作成target finalized file読戻し");
    if (!after ||
        !same_opened_object(file->second.created_identity, after.value()) ||
        after.value().size_bytes != entry.size_bytes ||
        after.value().creation_time != entry.creation_time_utc_100ns ||
        after.value().last_write_time != entry.last_write_time_utc_100ns ||
        portable_attributes(after.value().attributes) !=
            entry.portable_attributes) {
      return after
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"再作成target finalized file metadata読戻し",
                L"FileId、EOF、portable times、attributes、またはnon-reparse証跡が一致しません")
          : clonecore::Status::failure(after.error());
    }
    file->second.finalized = true;
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status apply_directory_metadata_and_flush(
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto root = inspect_retained_root();
    const auto directory = directories_.find(entry.relative_path);
    if (!root || directory == directories_.end() ||
        entry.kind != migrationcore::FileSystemRecreateEntryKind::directory) {
      return !root
          ? clonecore::Status::failure(root.error())
          : status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_NOT_FOUND,
                L"再作成target directory metadata token",
                L"retained directory handleを一意に取得できません");
    }
    auto before = inspect_opened_handle(
        directory->second.get(), true, L"再作成target directory metadata前");
    if (!before) {
      return clonecore::Status::failure(before.error());
    }
    auto status = apply_portable_metadata(
        directory->second.get(),
        entry,
        L"再作成target directory metadata/write-through",
        false);
    if (!status) {
      return status;
    }
    auto after = inspect_opened_handle(
        directory->second.get(), true, L"再作成target directory metadata後");
    if (!after || !same_opened_object(before.value(), after.value()) ||
        after.value().creation_time != entry.creation_time_utc_100ns ||
        after.value().last_write_time != entry.last_write_time_utc_100ns ||
        portable_attributes(after.value().attributes) !=
            entry.portable_attributes) {
      return after
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"再作成target directory metadata読戻し",
                L"FileId、portable times、attributes、またはnon-reparse証跡が一致しません")
          : clonecore::Status::failure(after.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status flush_target_namespace() override {
    auto root = inspect_retained_root();
    if (!root) {
      return clonecore::Status::failure(root.error());
    }
    for (const auto& [token, file] : files_) {
      static_cast<void>(token);
      if (!file.finalized || FlushFileBuffers(file.handle.get()) == FALSE) {
        return status_failure(
            clonecore::ErrorCode::io_failed,
            file.finalized ? GetLastError() : ERROR_INVALID_STATE,
            L"再作成target retained file flush",
            L"全fileのfinalize/flush証跡が揃っていません");
      }
    }
    for (const auto& [path, directory] : directories_) {
      auto identity = inspect_opened_handle(
          directory.get(), true, L"再作成target retained directory再照合");
      if (!identity) {
        return clonecore::Status::failure(identity.error());
      }
      static_cast<void>(path);
    }
    auto root_identity = inspect_opened_handle(
        root_.get(), true, L"再作成target retained root flush前再照合");
    if (!root_identity ||
        !same_opened_object(*root_identity_, root_identity.value())) {
      return root_identity
          ? status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"再作成target retained root flush前再照合",
                L"root FileIdまたはVolume serialが初期bindingから変化しました")
          : clonecore::Status::failure(root_identity.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<
      migrationcore::FileSystemRecreateTargetReadback>
  enumerate_complete_target_readback_read_only(
      const migrationcore::FileSystemRecreatePlan& plan) override {
    auto flushed = flush_target_namespace();
    if (!flushed) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateTargetReadback>::failure(
          flushed.error());
    }
    std::vector<std::byte> token_bytes;
    token_bytes.reserve(
        binding_->canonical_volume_guid_path.size() * sizeof(wchar_t) + 24U);
    for (const wchar_t value : binding_->canonical_volume_guid_path) {
      const auto unit = static_cast<std::uint16_t>(value);
      token_bytes.push_back(
          std::byte{static_cast<std::uint8_t>(unit & 0xFFU)});
      token_bytes.push_back(
          std::byte{static_cast<std::uint8_t>(unit >> 8U)});
    }
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
      token_bytes.push_back(std::byte{static_cast<std::uint8_t>(
          (binding_->target_offset >> shift) & 0xFFU)});
      token_bytes.push_back(std::byte{static_cast<std::uint8_t>(
          (binding_->target_size >> shift) & 0xFFU)});
    }
    auto token = imageformat::sha256(token_bytes);
    if (!token) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateTargetReadback>::failure(
          token.error());
    }
    const auto epoch = token.value();
    WindowsFileSystemRecreateSourceRequest request{
        .expected_source_disk = expected_target_,
        .source_table_index = binding_->final_target_number,
        .source_partition_offset_bytes = binding_->target_offset,
        .source_partition_length_bytes = binding_->target_size,
        .expected_file_system = geometry_->file_system,
        .source_root_path = binding_->canonical_volume_guid_path,
        .expected_source_epoch_token_sha256 = epoch,
        .observe_source_epoch_token = [this, epoch]() {
          auto root = inspect_retained_root();
          return root
              ? clonecore::Result<
                    migrationcore::FileSystemRecreateSha256>::success(epoch)
              : clonecore::Result<
                    migrationcore::FileSystemRecreateSha256>::failure(
                    root.error());
        },
        .limits = WindowsFileSystemRecreateSourceLimits{
            .maximum_entries = migrationcore::
                kMaximumFileSystemRecreateEntries,
            .maximum_depth = kDefaultWindowsFileSystemRecreateMaximumDepth,
            .maximum_path_utf16_units =
                geometry_->maximum_path_utf16_units,
            .maximum_file_bytes = geometry_->file_system ==
                    migrationcore::MigrationFileSystem::fat32
                ? migrationcore::kFat32MaximumRecreatedFileBytes
                : migrationcore::kExfatMaximumRecreatedFileBytes,
        },
    };
    auto session =
        open_windows_file_system_recreate_source_session_read_only(request);
    if (!session) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateTargetReadback>::failure(
          session.error());
    }
    auto tree = session.value()->canonical_tree();
    tree.source_table_index = plan.source_table_index();
    return clonecore::Result<
        migrationcore::FileSystemRecreateTargetReadback>::success({
        .target_partition_number = plan.target_partition_number(),
        .target_partition_offset_bytes =
            plan.target_partition_offset_bytes(),
        .actual_geometry = *geometry_,
        .target_tree = std::move(tree),
    });
  }

  [[nodiscard]] clonecore::Status close_namespace_dismount_and_offline(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume)
      override {
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return exact;
    }
    close_namespace_handles();
    clonecore::UniqueHandle handle(CreateFileW(
        trim_volume_slash(volume.canonical_volume_guid_path).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::access_denied,
          L"再作成target Volume lock open",
          GetLastError()));
    }
    auto extent = query_exact_volume_extent(handle.get());
    if (!extent || extent.value().disk_number != volume.disk_number ||
        extent.value().offset != volume.target_offset ||
        extent.value().length != volume.target_size) {
      return extent
          ? status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"再作成target dismount直前extent",
                L"lock対象Volume extentがレビュー済み範囲と一致しません")
          : clonecore::Status::failure(extent.error());
    }
    DWORD returned = 0U;
    // Child directories were created with FILE_WRITE_THROUGH and every file
    // was explicitly flushed while its exact handle was retained.  The
    // documented volume-handle flush now closes the whole-volume durability
    // boundary before lock/dismount/offline.
    if (FlushFileBuffers(handle.get()) == FALSE ||
        DeviceIoControl(
            handle.get(),
            FSCTL_LOCK_VOLUME,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) == FALSE ||
        DeviceIoControl(
            handle.get(),
            FSCTL_DISMOUNT_VOLUME,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) == FALSE ||
        DeviceIoControl(
            handle.get(),
            IOCTL_VOLUME_OFFLINE,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) == FALSE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"再作成target Volume lock/dismount/offline",
          GetLastError()));
    }
    handle.reset();
    auto offline = set_target_offline(true);
    if (!offline) {
      return offline;
    }
    auto observed = observe_target_read_only();
    if (!observed ||
        !observed.value().physical.target.offline.value_or(false)) {
      return observed
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_STATE,
                L"再作成target physical offline読戻し",
                L"dismount後のphysical targetをofflineと証明できません")
          : clonecore::Status::failure(observed.error());
    }
    return clonecore::success_status();
  }

 private:
  [[nodiscard]] clonecore::Result<HANDLE> parent_handle(
      const std::wstring& relative_path) const {
    const auto parent = parent_path(relative_path);
    if (parent.empty()) {
      return root_
          ? clonecore::Result<HANDLE>::success(root_.get())
          : failure<HANDLE>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_HANDLE,
                L"再作成target root parent handle",
                L"retained Volume root handleがありません");
    }
    const auto found = directories_.find(parent);
    return found != directories_.end()
        ? clonecore::Result<HANDLE>::success(found->second.get())
        : failure<HANDLE>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_PATH_NOT_FOUND,
              L"再作成target directory parent handle",
              L"親directoryのretained handleがありません");
  }

  [[nodiscard]] clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  inspect_retained_root() const {
    if (!root_ || !root_identity_ || !binding_ || !geometry_) {
      return failure<WindowsFileSystemRecreateFormattedRootObservation>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_HANDLE,
          L"再作成target retained root状態",
          L"Volume GUID/root/geometryの完全なretained bindingがありません");
    }
    auto exact = validate_exact_binding(*binding_);
    if (!exact) {
      return clonecore::Result<
          WindowsFileSystemRecreateFormattedRootObservation>::failure(
          exact.error());
    }
    auto identity = inspect_opened_handle(
        root_.get(), true, L"再作成target retained root FileId再照合");
    if (!identity ||
        !same_opened_object(*root_identity_, identity.value())) {
      return identity
          ? failure<WindowsFileSystemRecreateFormattedRootObservation>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"再作成target retained root FileId再照合",
                L"root FileIdまたはVolume serialが初期bindingから変化しました")
          : clonecore::Result<
                WindowsFileSystemRecreateFormattedRootObservation>::failure(
                identity.error());
    }
    std::array<wchar_t, MAX_PATH + 1U> volume_label{};
    std::array<wchar_t, 32U> file_system{};
    DWORD serial = 0U;
    DWORD maximum_component = 0U;
    DWORD flags = 0U;
    DWORD sectors_per_cluster = 0U;
    DWORD bytes_per_sector = 0U;
    DWORD free_clusters = 0U;
    DWORD total_clusters = 0U;
    if (GetVolumeInformationByHandleW(
            root_.get(),
            volume_label.data(),
            static_cast<DWORD>(volume_label.size()),
            &serial,
            &maximum_component,
            &flags,
            file_system.data(),
            static_cast<DWORD>(file_system.size())) == FALSE ||
        GetDiskFreeSpaceW(
            binding_->canonical_volume_guid_path.c_str(),
            &sectors_per_cluster,
            &bytes_per_sector,
            &free_clusters,
            &total_clusters) == FALSE) {
      return clonecore::Result<
          WindowsFileSystemRecreateFormattedRootObservation>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"再作成target actual filesystem geometry",
              GetLastError()));
    }
    const wchar_t* expected = geometry_->file_system ==
            migrationcore::MigrationFileSystem::fat32
        ? L"FAT32"
        : L"exFAT";
    if (volume_label.front() != L'\0' ||
        !same_text_case_insensitive(file_system.data(), expected) ||
        bytes_per_sector != geometry_->logical_sector_size ||
        sectors_per_cluster == 0U ||
        static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector !=
            geometry_->cluster_size ||
        maximum_component != geometry_->maximum_component_utf16_units) {
      return failure<WindowsFileSystemRecreateFormattedRootObservation>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"再作成target actual filesystem geometry",
          L"空label、filesystem、logical sector、allocation unit、またはcomponent上限が固定計画と一致しません");
    }
    return clonecore::Result<
        WindowsFileSystemRecreateFormattedRootObservation>::success({
        .volume = *binding_,
        .actual_geometry = *geometry_,
        .root_reparse_tag = identity.value().reparse_tag,
        .root_is_directory = identity.value().directory,
        .root_opened_handle_identity_stable = true,
    });
  }

  void close_namespace_handles() noexcept {
    files_.clear();
    directories_.clear();
    root_.reset();
    root_identity_.reset();
    binding_.reset();
    geometry_.reset();
  }

  clonecore::StableDiskIdentity expected_target_;
  clonecore::TargetConfirmation confirmation_;
  bool active_rescue_media_{};
  clonecore::DiskOperationCallbacks callbacks_;
  clonecore::UniqueHandle root_;
  std::optional<OpenedHandleIdentity> root_identity_;
  std::optional<WindowsFileSystemRecreateConstructionVolumeBinding> binding_;
  std::optional<migrationcore::FileSystemRecreateFormatGeometry> geometry_;
  std::map<std::wstring, clonecore::UniqueHandle, OrdinalCaseInsensitiveLess>
      directories_;
  std::map<std::uint64_t, FileState> files_;
  std::uint64_t next_file_token_{1U};
};

}  // namespace

clonecore::Result<std::unique_ptr<
    IWindowsFileSystemRecreateProductionTargetPlatform>>
make_windows_file_system_recreate_target_platform(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const WindowsFileSystemRecreateProductionTargetRequest& request) {
  try {
    auto io = std::make_unique<Win32FileSystemRecreateTargetIo>(
        plan.target().expected_target_disk,
        request.confirmation,
        request.target_is_active_rescue_media,
        request.callbacks);
    return make_windows_file_system_recreate_target_platform_with_io(
        plan, request, std::move(io));
  } catch (...) {
    return failure<std::unique_ptr<
        IWindowsFileSystemRecreateProductionTargetPlatform>>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"再作成Win32 target factory allocation",
        L"Win32 production target I/O adapterを生成できませんでした");
  }
}

}  // namespace ytec::migrationengine

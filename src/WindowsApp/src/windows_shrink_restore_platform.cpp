#include "ytec/windowsapp/windows_shrink_restore_platform.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/imageformat/tsumugi_restore_layout_io.h"
#include "ytec/windowsdism/dism.h"

#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
#include <vds.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kIoBlockBytes = 1024U * 1024U;
constexpr std::uint32_t kVolumeArrivalAttempts = 120U;
constexpr DWORD kWimGenericRead = 0x80000000UL;
constexpr DWORD kWimOpenExisting = 3U;
constexpr DWORD kWimFlagVerify = 0x00000002UL;
// CLSID_VdsLoader from the Windows SDK. Keeping the one class identifier local
// avoids INITGUID definitions for the entire VDS header and any cross-library
// ODR coupling with the existing WinPE VDS translation unit.
constexpr CLSID kVdsLoaderClassId{
    0x9C38ED61U,
    0xD565U,
    0x4728U,
    {0xAEU, 0xEEU, 0xC8U, 0x09U, 0x52U, 0xF0U, 0xECU, 0xDEU},
};

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
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(platform_error(
      code, native_code, std::move(operation), std::move(message)));
}

template <typename Range>
bool all_zero(const Range& bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const auto value) {
    return value == std::byte{0};
  });
}

bool same_restore_identity(
    const imageformat::TsumugiRestoreDiskIdentity& left,
    const imageformat::TsumugiRestoreDiskIdentity& right) noexcept {
  return left.stable_identity_hash == right.stable_identity_hash &&
      left.disk_size == right.disk_size &&
      left.logical_sector_size == right.logical_sector_size &&
      left.is_running_windows_system_disk ==
          right.is_running_windows_system_disk &&
      left.is_usb_attached == right.is_usb_attached &&
      left.is_usb_memory == right.is_usb_memory &&
      left.is_active_rescue_media == right.is_active_rescue_media &&
      left.is_dynamic_disk == right.is_dynamic_disk &&
      left.is_storage_spaces == right.is_storage_spaces &&
      left.is_windows_software_raid == right.is_windows_software_raid &&
      left.has_unresolved_hardware_raid ==
          right.has_unresolved_hardware_raid &&
      left.connection_instance_hash == right.connection_instance_hash;
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool valid_file_system(
    const imageformat::TsumugiManifestFileSystem file_system) noexcept {
  return file_system == imageformat::TsumugiManifestFileSystem::ntfs ||
      file_system == imageformat::TsumugiManifestFileSystem::exfat ||
      file_system == imageformat::TsumugiManifestFileSystem::fat32;
}

std::wstring trim_volume_slash(std::wstring path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
    path.pop_back();
  }
  return path;
}

clonecore::Result<std::wstring> system_directory() {
  std::vector<wchar_t> buffer(32768U, L'\0');
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"信頼済みSystem32取得",
            length == 0U ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(buffer.data(), length));
}

class LocalMemory final {
 public:
  LocalMemory() noexcept = default;
  explicit LocalMemory(void* value) noexcept : value_(value) {}
  ~LocalMemory() { reset(); }
  LocalMemory(const LocalMemory&) = delete;
  LocalMemory& operator=(const LocalMemory&) = delete;
  LocalMemory(LocalMemory&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  LocalMemory& operator=(LocalMemory&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] void* get() const noexcept { return value_; }

 private:
  void reset() noexcept {
    if (value_ != nullptr) {
      LocalFree(value_);
      value_ = nullptr;
    }
  }
  void* value_{};
};

class ScopedTokenPrivilege final {
 public:
  ScopedTokenPrivilege() noexcept = default;
  ScopedTokenPrivilege(
      clonecore::UniqueHandle token,
      const TOKEN_PRIVILEGES& previous,
      const DWORD previous_size) noexcept
      : token_(std::move(token)),
        previous_(previous),
        previous_size_(previous_size),
        restore_(true) {}
  ~ScopedTokenPrivilege() { reset(); }
  ScopedTokenPrivilege(const ScopedTokenPrivilege&) = delete;
  ScopedTokenPrivilege& operator=(const ScopedTokenPrivilege&) = delete;
  ScopedTokenPrivilege(ScopedTokenPrivilege&& other) noexcept
      : token_(std::move(other.token_)),
        previous_(other.previous_),
        previous_size_(other.previous_size_),
        restore_(std::exchange(other.restore_, false)) {}
  ScopedTokenPrivilege& operator=(ScopedTokenPrivilege&& other) noexcept {
    if (this != &other) {
      reset();
      token_ = std::move(other.token_);
      previous_ = other.previous_;
      previous_size_ = other.previous_size_;
      restore_ = std::exchange(other.restore_, false);
    }
    return *this;
  }

 private:
  void reset() noexcept {
    if (restore_ && token_) {
      static_cast<void>(AdjustTokenPrivileges(
          token_.get(),
          FALSE,
          &previous_,
          previous_size_,
          nullptr,
          nullptr));
    }
    restore_ = false;
    token_.reset();
  }

  clonecore::UniqueHandle token_;
  TOKEN_PRIVILEGES previous_{};
  DWORD previous_size_{};
  bool restore_{};
};

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

clonecore::Result<ScopedTokenPrivilege> enable_process_privilege(
    const wchar_t* privilege_name,
    const std::wstring_view operation) {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(
          GetCurrentProcess(),
          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
          &raw_token)) {
    return clonecore::Result<ScopedTokenPrivilege>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            operation,
            GetLastError()));
  }
  clonecore::UniqueHandle token(raw_token);
  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, privilege_name, &luid)) {
    return clonecore::Result<ScopedTokenPrivilege>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            operation,
            GetLastError()));
  }
  TOKEN_PRIVILEGES requested{};
  requested.PrivilegeCount = 1U;
  requested.Privileges[0].Luid = luid;
  requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  TOKEN_PRIVILEGES previous{};
  DWORD previous_size = sizeof(previous);
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(
      token.get(),
      FALSE,
      &requested,
      sizeof(previous),
      &previous,
      &previous_size);
  const DWORD error = GetLastError();
  if (adjusted == FALSE || error == ERROR_NOT_ALL_ASSIGNED) {
    return clonecore::Result<ScopedTokenPrivilege>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            operation,
            adjusted == FALSE ? error : ERROR_PRIVILEGE_NOT_HELD));
  }
  return clonecore::Result<ScopedTokenPrivilege>::success(
      ScopedTokenPrivilege(std::move(token), previous, previous_size));
}

clonecore::Result<LocalMemory> restricted_security_descriptor() {
  clonecore::UniqueHandle token;
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            L"縮小復元WIM所有者Token取得",
            GetLastError()));
  }
  token.reset(raw_token);
  DWORD bytes = 0U;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0U, &bytes);
  if (bytes == 0U || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小復元WIM所有者SID寸法",
            GetLastError()));
  }
  std::vector<std::byte> user_bytes(bytes);
  if (!GetTokenInformation(
          token.get(), TokenUser, user_bytes.data(), bytes, &bytes)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小復元WIM所有者SID取得",
            GetLastError()));
  }
  const auto* user = reinterpret_cast<const TOKEN_USER*>(user_bytes.data());
  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小復元WIM所有者SID変換",
            GetLastError()));
  }
  LocalMemory sid(sid_text);
  const std::wstring owner(static_cast<const wchar_t*>(sid.get()));
  const std::wstring sddl = L"O:" + owner + L"D:P(A;;FA;;;" + owner +
      L")(A;;FA;;;SY)(A;;FA;;;BA)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::internal_error,
            L"縮小復元WIM保護DACL作成",
            GetLastError()));
  }
  return clonecore::Result<LocalMemory>::success(LocalMemory(descriptor));
}

clonecore::Status verify_regular_single_link(
    const HANDLE handle,
    const std::uint64_t expected_length,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::verification_failed,
        operation,
        GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      standard.NumberOfLinks != 1U || standard.DeletePending != FALSE ||
      standard.EndOfFile.QuadPart < 0 ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) !=
          expected_length) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_INVALID,
        std::wstring(operation),
        L"所有WIMが通常単一linkファイルではないか、長さが変化しました");
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_file_exact(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::size_t length) {
  if (offset > static_cast<std::uint64_t>(
                   (std::numeric_limits<LONGLONG>::max)()) ||
      length > (std::numeric_limits<DWORD>::max)()) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小復元WIM読戻し範囲",
        L"読戻し範囲をWindows APIで表現できません");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"縮小復元WIM読戻し位置",
            GetLastError()));
  }
  std::vector<std::byte> bytes(length);
  DWORD read = 0U;
  if (length != 0U &&
      (!ReadFile(
           handle, bytes.data(), static_cast<DWORD>(length), &read, nullptr) ||
       read != length)) {
    const DWORD error = GetLastError();
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::io_failed,
        error == ERROR_SUCCESS ? ERROR_HANDLE_EOF : error,
        L"縮小復元WIM読戻し",
        L"同一WIM handleから要求長を読み戻せません");
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Result<std::uint64_t> read_regular_file_to_stable_eof(
    const std::wstring& path,
    const clonecore::DiskOperationCallbacks& callbacks) {
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
    return clonecore::Result<std::uint64_t>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::verification_failed,
            L"縮小復元全通常ファイル読戻しopen",
            GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO before{};
  if (!GetFileInformationByHandleEx(
          file.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          file.get(), FileStandardInfo, &before, sizeof(before))) {
    return clonecore::Result<std::uint64_t>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::verification_failed,
            L"縮小復元全通常ファイルmetadata読戻し",
            GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      before.DeletePending != FALSE || before.EndOfFile.QuadPart < 0) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_INVALID,
        L"縮小復元全通常ファイル種別",
        L"列挙後に通常ファイルの種別または長さが変化しました");
  }
  const auto expected = static_cast<std::uint64_t>(before.EndOfFile.QuadPart);
  std::vector<std::byte> buffer(kIoBlockBytes);
  std::uint64_t delivered = 0U;
  while (delivered < expected) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小復元全通常ファイル読戻し取消",
          L"通常ファイルの境界で安全に取り消しました");
    }
    const auto request = static_cast<DWORD>(
        (std::min<std::uint64_t>)(expected - delivered, buffer.size()));
    DWORD read = 0U;
    if (!ReadFile(file.get(), buffer.data(), request, &read, nullptr)) {
      return clonecore::Result<std::uint64_t>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::verification_failed,
              L"縮小復元全通常ファイル内容読戻し",
              GetLastError()));
    }
    if (read == 0U || read > request) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::verification_failed,
          ERROR_HANDLE_EOF,
          L"縮小復元全通常ファイルEOF",
          L"記録済みEOFより前に読戻しが終了しました");
    }
    delivered += read;
  }
  std::byte trailing{};
  DWORD trailing_read = 0U;
  if (!ReadFile(file.get(), &trailing, 1U, &trailing_read, nullptr) ||
      trailing_read != 0U) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_INVALID,
        L"縮小復元全通常ファイルEOF再確認",
        L"読戻し中に通常ファイルが伸長した可能性があります");
  }
  FILE_STANDARD_INFO after{};
  if (!GetFileInformationByHandleEx(
          file.get(), FileStandardInfo, &after, sizeof(after)) ||
      after.DeletePending != FALSE ||
      after.EndOfFile.QuadPart != before.EndOfFile.QuadPart ||
      after.NumberOfLinks != before.NumberOfLinks) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_INVALID,
        L"縮小復元全通常ファイル安定EOF",
        L"読戻し中に通常ファイルの長さまたはlink状態が変化しました");
  }
  return clonecore::Result<std::uint64_t>::success(delivered);
}

clonecore::Result<imageformat::Sha256Digest> hash_file_handle(
    const HANDLE handle,
    const std::uint64_t length,
    const clonecore::DiskOperationCallbacks& callbacks) {
  return imageformat::sha256_from_reader(
      length,
      kIoBlockBytes,
      [handle, &callbacks](
          const std::uint64_t offset, const std::size_t bytes) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return failure<std::vector<std::byte>>(
              clonecore::ErrorCode::cancelled,
              ERROR_CANCELLED,
              L"縮小復元WIM Hash取消",
              L"WIM読取りblock境界で安全に取り消しました");
        }
        return read_file_exact(handle, offset, bytes);
      });
}

struct ExactVolumeExtent final {
  std::uint32_t disk_number{};
  std::uint64_t offset{};
  std::uint64_t length{};
};

clonecore::Result<ExactVolumeExtent> query_exact_volume_extent(
    const std::wstring& volume_device_path) {
  const std::wstring open_path = trim_volume_slash(volume_device_path);
  clonecore::UniqueHandle volume(CreateFileW(
      open_path.c_str(),
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
            L"縮小復元Volume extent照会open",
            GetLastError()));
  }
  std::vector<std::byte> buffer(64U * 1024U);
  DWORD returned = 0U;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0U,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &returned,
          nullptr)) {
    return clonecore::Result<ExactVolumeExtent>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小復元Volume extent照会",
            GetLastError()));
  }
  constexpr std::size_t header = offsetof(VOLUME_DISK_EXTENTS, Extents);
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (returned < header + sizeof(DISK_EXTENT) ||
      extents->NumberOfDiskExtents != 1U ||
      extents->Extents[0].StartingOffset.QuadPart < 0 ||
      extents->Extents[0].ExtentLength.QuadPart <= 0) {
    return failure<ExactVolumeExtent>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小復元Volume extent一意性",
        L"対象Volumeが単一の正しい物理範囲ではありません");
  }
  return clonecore::Result<ExactVolumeExtent>::success({
      .disk_number = extents->Extents[0].DiskNumber,
      .offset = static_cast<std::uint64_t>(
          extents->Extents[0].StartingOffset.QuadPart),
      .length = static_cast<std::uint64_t>(
          extents->Extents[0].ExtentLength.QuadPart),
  });
}

clonecore::Status validate_exact_binding(
    const WindowsTsumugiShrinkVolumeBinding& binding) {
  auto extent = query_exact_volume_extent(binding.volume_device_path);
  if (!extent) {
    return clonecore::Status::failure(extent.error());
  }
  if (extent.value().disk_number != binding.disk_number ||
      extent.value().offset != binding.target_offset ||
      extent.value().length != binding.target_size) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"縮小復元Volume物理範囲再照合",
        L"Volumeのディスク、開始位置、または長さがレビュー済み配置と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint64_t> query_ntfs_volume_bytes(
    const HANDLE volume,
    const std::uint64_t expected_cluster_size,
    const std::wstring_view operation) {
  NTFS_VOLUME_DATA_BUFFER data{};
  DWORD returned = 0U;
  if (!DeviceIoControl(
          volume,
          FSCTL_GET_NTFS_VOLUME_DATA,
          nullptr,
          0U,
          &data,
          sizeof(data),
          &returned,
          nullptr)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        GetLastError(),
        std::wstring(operation),
        L"NTFS volume dataを取得できません");
  }
  if (returned < offsetof(NTFS_VOLUME_DATA_BUFFER, BytesPerFileRecordSegment) ||
      data.NumberSectors.QuadPart <= 0 || data.BytesPerSector == 0U ||
      data.BytesPerCluster == 0U ||
      data.BytesPerCluster != expected_cluster_size) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"NTFS sector数、sector寸法、またはcluster寸法を検証できません");
  }
  std::uint64_t bytes{};
  if (!checked_multiply(
          static_cast<std::uint64_t>(data.NumberSectors.QuadPart),
          data.BytesPerSector,
          bytes)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"NTFS sector数とsector寸法の積がオーバーフローしました");
  }
  return clonecore::Result<std::uint64_t>::success(bytes);
}

clonecore::Status format_exact_volume_with_vds(
    const WindowsTsumugiShrinkVolumeBinding& binding,
    const imageformat::TsumugiManifestFileSystem file_system,
    const std::uint64_t cluster_size) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool must_uninitialize = initialized == S_OK || initialized == S_FALSE;
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return status_failure(
        clonecore::ErrorCode::unsupported_platform,
        static_cast<DWORD>(initialized),
        L"縮小復元VDS COM初期化",
        L"Microsoft Virtual Disk Service formatterを初期化できません");
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
        L"縮小復元VDS loader取得",
        L"Microsoft VDS loaderを取得できません");
  }
  UniqueComInterface<IVdsService> service;
  result = loader->LoadService(nullptr, service.put());
  if (result != S_OK || !service) {
    return status_failure(
        clonecore::ErrorCode::unsupported_platform,
        static_cast<DWORD>(result),
        L"縮小復元VDS service取得",
        L"Microsoft VDS serviceを読み込めません");
  }
  result = service->WaitForServiceReady();
  if (result != S_OK) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"縮小復元VDS service準備",
        L"Microsoft VDS serviceの準備完了を確認できません");
  }
  result = service->Reenumerate();
  if (result == S_OK) {
    result = service->Refresh();
  }
  if (result != S_OK) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"縮小復元VDS一時Volume再列挙",
        L"一時VolumeをVDSへ再列挙できません");
  }

  UniqueComInterface<IEnumVdsObject> providers;
  result = service->QueryProviders(
      VDS_QUERY_SOFTWARE_PROVIDERS, providers.put());
  if (result != S_OK || !providers) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"縮小復元VDS software provider列挙",
        L"Microsoft software providerを列挙できません");
  }
  const std::wstring expected_name =
      trim_volume_slash(binding.volume_device_path);
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
          L"縮小復元VDS provider取得",
          L"VDS provider列挙結果が一意ではありません");
    }
    UniqueComInterface<IUnknown> provider_unknown(provider_raw);
    UniqueComInterface<IVdsSwProvider> provider;
    result = provider_unknown->QueryInterface(IID_PPV_ARGS(provider.put()));
    if (result != S_OK || !provider) {
      return status_failure(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"縮小復元VDS provider照合",
          L"列挙objectをsoftware providerとして照合できません");
    }
    UniqueComInterface<IEnumVdsObject> packs;
    result = provider->QueryPacks(packs.put());
    if (result != S_OK || !packs) {
      return status_failure(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"縮小復元VDS pack列挙",
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
            L"縮小復元VDS pack取得",
            L"VDS pack列挙結果が一意ではありません");
      }
      UniqueComInterface<IUnknown> pack_unknown(pack_raw);
      UniqueComInterface<IVdsPack> pack;
      result = pack_unknown->QueryInterface(IID_PPV_ARGS(pack.put()));
      if (result != S_OK || !pack) {
        return status_failure(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"縮小復元VDS pack照合",
            L"列挙objectをVDS packとして照合できません");
      }
      UniqueComInterface<IEnumVdsObject> volumes;
      result = pack->QueryVolumes(volumes.put());
      if (result != S_OK || !volumes) {
        return status_failure(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"縮小復元VDS volume列挙",
            L"VDS packのvolumeを列挙できません");
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
              L"縮小復元VDS volume取得",
              L"VDS volume列挙結果が一意ではありません");
        }
        UniqueComInterface<IUnknown> volume_unknown(volume_raw);
        UniqueComInterface<IVdsVolume> volume;
        result = volume_unknown->QueryInterface(IID_PPV_ARGS(volume.put()));
        if (result != S_OK || !volume) {
          return status_failure(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"縮小復元VDS volume照合",
              L"列挙objectをVDS volumeとして照合できません");
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
              L"縮小復元VDS volume属性",
              L"VDS volume属性を取得できません");
        }
        if (_wcsicmp(name.c_str(), expected_name.c_str()) != 0) {
          continue;
        }
        constexpr ULONG forbidden_flags =
            VDS_VF_SYSTEM_VOLUME | VDS_VF_BOOT_VOLUME | VDS_VF_PAGEFILE |
            VDS_VF_HIBERNATION | VDS_VF_CRASHDUMP |
            VDS_VF_NOT_FORMATTABLE | VDS_VF_SHADOW_COPY |
            VDS_VF_FVE_ENABLED | VDS_VF_BACKS_BOOT_VOLUME |
            VDS_VF_BACKED_BY_WIM_IMAGE;
        const bool known_failed_health =
            properties.health == VDS_H_FAILING ||
            properties.health == VDS_H_PENDING_FAILURE ||
            properties.health == VDS_H_FAILED;
        if (formatter || properties.type != VDS_VT_SIMPLE ||
            properties.status != VDS_VS_ONLINE ||
            properties.TransitionState != VDS_TS_STABLE ||
            properties.ullSize != binding.target_size ||
            (properties.ulFlags & forbidden_flags) != 0U ||
            known_failed_health) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"縮小復元VDS exact volume拘束",
              L"VDS volumeの一意性、寸法、状態、または安全属性が一致しません");
        }
        result = volume->QueryInterface(IID_PPV_ARGS(formatter.put()));
        if (result != S_OK || !formatter) {
          return status_failure(
              clonecore::ErrorCode::unsupported_platform,
              static_cast<DWORD>(result),
              L"縮小復元VDS FormatEx取得",
              L"対象volumeにMicrosoft VDS FormatExがありません");
        }
      }
    }
  }
  if (!formatter) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"縮小復元VDS exact volume検索",
        L"exact extentで拘束した一時VolumeをVDSで一意に特定できません");
  }
  auto exact = validate_exact_binding(binding);
  if (!exact) {
    return exact;
  }
  std::wstring file_system_name =
      file_system == imageformat::TsumugiManifestFileSystem::ntfs
      ? L"NTFS"
      : file_system == imageformat::TsumugiManifestFileSystem::exfat
            ? L"EXFAT"
            : L"FAT32";
  std::wstring empty_label;
  UniqueComInterface<IVdsAsync> asynchronous;
  result = formatter->FormatEx(
      file_system_name.data(),
      0U,
      static_cast<ULONG>(cluster_size),
      empty_label.data(),
      TRUE,
      TRUE,
      FALSE,
      asynchronous.put());
  if (result != S_OK || !asynchronous) {
    return status_failure(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(result),
        L"縮小復元VDS FormatEx開始",
        L"Microsoft VDS quick formatを開始できません");
  }
  HRESULT operation_result = E_FAIL;
  VDS_ASYNC_OUTPUT output{};
  result = asynchronous->Wait(&operation_result, &output);
  if (result != S_OK || operation_result != S_OK ||
      output.type != VDS_ASYNCOUT_FORMAT) {
    return status_failure(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(
            result != S_OK ? result : operation_result),
        L"縮小復元VDS FormatEx完了",
        L"Microsoft VDS quick formatの完了を確認できません");
  }
  return validate_exact_binding(binding);
}

clonecore::Status write_handle_exact(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes) {
  if (offset > static_cast<std::uint64_t>(
                   (std::numeric_limits<LONGLONG>::max)()) ||
      bytes.size() > (std::numeric_limits<DWORD>::max)()) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小復元WIM書込み範囲",
        L"WIM書込み範囲をWindows APIで表現できません");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"縮小復元WIM書込み位置",
        GetLastError()));
  }
  DWORD written = 0U;
  if (!bytes.empty() &&
      (!WriteFile(
           handle,
           bytes.data(),
           static_cast<DWORD>(bytes.size()),
           &written,
           nullptr) ||
       written != bytes.size())) {
    const DWORD error = GetLastError();
    return status_failure(
        clonecore::ErrorCode::io_failed,
        error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error,
        L"縮小復元WIM書込み",
        L"所有WIMへ要求長を書き込めません");
  }
  return clonecore::success_status();
}

clonecore::Status verify_wim_image_count(
    const std::wstring& path,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust) {
  std::wstring library_path = trusted_system_directory;
  if (!library_path.ends_with(L'\\')) {
    library_path.push_back(L'\\');
  }
  library_path.append(L"wimgapi.dll");
  auto status = trust.verify_microsoft_signed(library_path);
  if (!status) {
    return status;
  }
  HMODULE library = LoadLibraryExW(
      library_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
  if (library == nullptr) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::unsupported_platform,
        L"Microsoft WIMGAPI読込み",
        GetLastError()));
  }
  using CreateFileFunction = HANDLE(WINAPI*)(
      PCWSTR, DWORD, DWORD, DWORD, DWORD, PDWORD);
  using GetCountFunction = DWORD(WINAPI*)(HANDLE);
  using CloseFunction = BOOL(WINAPI*)(HANDLE);
  const auto create = reinterpret_cast<CreateFileFunction>(
      GetProcAddress(library, "WIMCreateFile"));
  const auto count = reinterpret_cast<GetCountFunction>(
      GetProcAddress(library, "WIMGetImageCount"));
  const auto close = reinterpret_cast<CloseFunction>(
      GetProcAddress(library, "WIMCloseHandle"));
  if (create == nullptr || count == nullptr || close == nullptr) {
    FreeLibrary(library);
    return status_failure(
        clonecore::ErrorCode::unsupported_platform,
        ERROR_PROC_NOT_FOUND,
        L"Microsoft WIMGAPI入口検証",
        L"単一WIM検証に必要な公式APIがありません");
  }
  DWORD creation_result = 0U;
  const HANDLE wim = create(
      path.c_str(),
      kWimGenericRead,
      kWimOpenExisting,
      kWimFlagVerify,
      0U,
      &creation_result);
  if (wim == nullptr || wim == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    FreeLibrary(library);
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::verification_failed,
        L"Microsoft WIMGAPI形式検証",
        error));
  }
  const DWORD image_count = count(wim);
  const DWORD count_error = GetLastError();
  const BOOL closed = close(wim);
  const DWORD close_error = GetLastError();
  status = trust.verify_microsoft_signed(library_path);
  FreeLibrary(library);
  if (!status) {
    return status;
  }
  if (image_count != 1U || closed == FALSE) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        image_count != 1U ? count_error : close_error,
        L"縮小復元単一WIM検証",
        L"WIMが有効な単一イメージではありません");
  }
  return clonecore::success_status();
}

clonecore::Result<std::wstring> make_staging_path(
    const std::wstring& scratch,
    const std::uint32_t source_table_index) {
  GUID identifier{};
  if (FAILED(CoCreateGuid(&identifier))) {
    return failure<std::wstring>(
        clonecore::ErrorCode::io_failed,
        ERROR_GEN_FAILURE,
        L"縮小復元WIM一意名生成",
        L"一時WIMの一意識別子を生成できません");
  }
  std::array<wchar_t, 64U> guid{};
  if (StringFromGUID2(identifier, guid.data(), static_cast<int>(guid.size())) <=
      0) {
    return failure<std::wstring>(
        clonecore::ErrorCode::io_failed,
        ERROR_GEN_FAILURE,
        L"縮小復元WIM一意名変換",
        L"一時WIMの一意識別子を文字列化できません");
  }
  std::wstring path = scratch;
  if (!path.ends_with(L'\\')) {
    path.push_back(L'\\');
  }
  path.append(L".ytec-shrink-")
      .append(std::to_wstring(source_table_index))
      .append(L"-")
      .append(guid.data())
      .append(L".wim");
  return clonecore::Result<std::wstring>::success(std::move(path));
}

class Win32ShrinkRestoreIo final
    : public IWindowsTsumugiShrinkRestorePlatformIo {
 public:
  explicit Win32ShrinkRestoreIo(
      WindowsTsumugiShrinkRestorePlatformRequest request)
      : request_(std::move(request)),
        trust_(bootrepair::make_windows_authenticode_verifier()),
        process_(bootrepair::make_windows_process_runner()) {}

  ~Win32ShrinkRestoreIo() override {
    static_cast<void>(discard_owned_staged_wim());
    staged_handle_.reset();
  }

  clonecore::Result<WindowsTsumugiShrinkTargetObservation>
  observe_original_target(
      const imageformat::Sha256Digest& connection_instance_hash) override {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto observed = diskmodel::reidentify_physical_target(
        request_.expected_target, request_.confirmation, *inventory);
    if (!observed) {
      return clonecore::Result<
          WindowsTsumugiShrinkTargetObservation>::failure(observed.error());
    }
    const auto target_class =
        imageformat::classify_tsumugi_physical_restore_target(
            observed.value().target);
    const auto accepted =
        imageformat::validate_tsumugi_physical_restore_target(
            observed.value().target,
            target_class,
            request_.target_is_active_rescue_media);
    auto layout = imageformat::hash_tsumugi_physical_restore_target_layout_v1(
        observed.value().target);
    auto identity_hash =
        imageformat::hash_tsumugi_physical_restore_target_identity_v1(
            observed.value().target_identity);
    if (!accepted || !layout || !identity_hash) {
      return clonecore::Result<
          WindowsTsumugiShrinkTargetObservation>::failure(
          !accepted ? accepted.error()
                    : !layout ? layout.error() : identity_hash.error());
    }
    if (layout.value() != request_.expected_target_layout_hash) {
      return failure<WindowsTsumugiShrinkTargetObservation>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Windows縮小復元先レイアウト再確認",
          L"OK確認後にコピー先のパーティション配置が変化しました");
    }
    if (target_class.usb_attached && all_zero(connection_instance_hash)) {
      return failure<WindowsTsumugiShrinkTargetObservation>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"Windows縮小USB接続Session",
          L"USB接続ディスクは現在の接続Sessionへ拘束できません");
    }
    return clonecore::Result<
        WindowsTsumugiShrinkTargetObservation>::success({
        .physical = observed.take_value(),
        .restore_identity = {
            .stable_identity_hash = identity_hash.take_value(),
            .disk_size = request_.expected_target.size_bytes,
            .logical_sector_size =
                request_.expected_target.logical_sector_size,
            .is_running_windows_system_disk =
                request_.expected_target.is_system_disk,
            .is_usb_attached = target_class.usb_attached,
            .is_usb_memory = target_class.usb_memory,
            .is_active_rescue_media =
                request_.target_is_active_rescue_media,
            .is_dynamic_disk = target_class.dynamic_disk,
            .is_storage_spaces = target_class.storage_spaces,
            .is_windows_software_raid = target_class.software_raid,
            .has_unresolved_hardware_raid =
                target_class.unresolved_hardware_raid,
            .connection_instance_hash = connection_instance_hash,
        },
    });
  }

  clonecore::Status validate_work_paths_disjoint(
      const WindowsShrinkWorkPaths& work_paths,
      const std::uint32_t current_target_disk_number) override {
    const auto validate = [&](const std::wstring& path,
                              const std::wstring_view role) {
      if (path.empty()) {
        return status_failure(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            std::wstring(role),
            L"作業パスが空です");
      }
      auto disk = diskmodel::query_single_disk_number_for_local_path(path);
      if (!disk) {
        return clonecore::Status::failure(disk.error());
      }
      if (disk.value() == current_target_disk_number) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DRIVE,
            std::wstring(role),
            L"作業ファイルを消去対象ディスクへ置くことはできません");
      }
      return clonecore::success_status();
    };
    auto status = validate(work_paths.scratch_directory, L"縮小復元scratch分離");
    if (!status) {
      return status;
    }
    status = validate(work_paths.checkpoint_path, L"縮小復元checkpoint分離");
    if (!status) {
      return status;
    }
    if (!work_paths.log_is_ram_only) {
      return validate(work_paths.log_path, L"縮小復元log分離");
    }
    return work_paths.log_path.empty()
        ? clonecore::success_status()
        : status_failure(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_NAME,
              L"縮小復元RAM log",
              L"RAM-only logにファイルパスを指定できません");
  }

  clonecore::Status set_target_offline(const bool offline) override {
    return diskmodel::set_verified_physical_target_offline_with_windows_apis(
        request_.expected_target, request_.confirmation, offline);
  }

  clonecore::Result<diskmodel::PhysicalTargetHandle>
  open_offline_target() override {
    return diskmodel::open_verified_physical_target_with_windows_apis(
        request_.expected_target, request_.confirmation);
  }

  clonecore::Status notify_layout_changed() override {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto observed = diskmodel::reidentify_physical_target(
        request_.expected_target, request_.confirmation, *inventory);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    clonecore::UniqueHandle disk(CreateFileW(
        observed.value().target.device_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!disk) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小復元レイアウト通知open",
          GetLastError()));
    }
    STORAGE_DEVICE_NUMBER number{};
    DWORD returned = 0U;
    if (!DeviceIoControl(
            disk.get(),
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0U,
            &number,
            sizeof(number),
            &returned,
            nullptr) ||
        returned < sizeof(number) ||
        number.DeviceType != FILE_DEVICE_DISK ||
        number.DeviceNumber != observed.value().target.disk_number) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"縮小復元レイアウト通知対象",
          L"再識別したPhysicalDriveと開いたhandleが一致しません");
    }
    if (!DeviceIoControl(
            disk.get(),
            IOCTL_DISK_UPDATE_PROPERTIES,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小復元レイアウト更新通知",
          GetLastError()));
    }
    auto verified = diskmodel::reidentify_physical_target(
        request_.expected_target, request_.confirmation, *inventory);
    return verified
        ? clonecore::success_status()
        : clonecore::Status::failure(verified.error());
  }

  clonecore::Result<WindowsTsumugiShrinkVolumeBinding> bind_online_volume(
      const std::uint32_t final_target_number,
      const std::uint64_t target_offset,
      const std::uint64_t target_size) override {
    std::optional<clonecore::Error> last_error;
    for (std::uint32_t attempt = 0U;
         attempt < kVolumeArrivalAttempts;
         ++attempt) {
      auto inventory = diskmodel::make_windows_disk_inventory_provider();
      auto observed = diskmodel::reidentify_physical_target(
          request_.expected_target, request_.confirmation, *inventory);
      if (!observed) {
        return clonecore::Result<
            WindowsTsumugiShrinkVolumeBinding>::failure(observed.error());
      }
      if (observed.value().target.offline.value_or(true)) {
        return failure<WindowsTsumugiShrinkVolumeBinding>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_STATE,
            L"縮小復元一時Volume online確認",
            L"仮GPT公開後の対象ディスクがonlineではありません");
      }
      const std::array<diskmodel::VolumePartitionLocation, 1U> expected{{
          {
              .table_index = final_target_number - 1U,
              .offset_bytes = target_offset,
          },
      }};
      auto bindings = diskmodel::query_windows_volume_bindings_by_offset(
          observed.value().target, expected);
      if (bindings && bindings.value().size() == 1U) {
        WindowsTsumugiShrinkVolumeBinding result{
            .final_target_number = final_target_number,
            .disk_number = observed.value().target.disk_number,
            .target_offset = target_offset,
            .target_size = target_size,
            .volume_device_path = bindings.value().front().volume_device_path,
        };
        const auto exact = validate_exact_binding(result);
        if (exact) {
          return clonecore::Result<
              WindowsTsumugiShrinkVolumeBinding>::success(
              std::move(result));
        }
        last_error = exact.error();
      } else if (!bindings) {
        last_error = bindings.error();
      }
      if (clonecore::disk_operation_cancellation_requested(
              request_.callbacks)) {
        return failure<WindowsTsumugiShrinkVolumeBinding>(
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED,
            L"縮小復元一時Volume待機",
            L"一時Volumeの到着待ちを安全な境界で取り消しました");
      }
      Sleep(250U);
    }
    return clonecore::Result<
        WindowsTsumugiShrinkVolumeBinding>::failure(
        last_error.value_or(platform_error(
            clonecore::ErrorCode::query_failed,
            ERROR_TIMEOUT,
            L"縮小復元一時Volume待機",
            L"一時Volumeを30秒以内に一意に対応付けできません")));
  }

  clonecore::Status format_volume(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      const imageformat::TsumugiManifestFileSystem file_system,
      const std::uint64_t cluster_size) override {
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return exact;
    }
    if (!valid_file_system(file_system) || cluster_size < 512U ||
        cluster_size > 32ULL * 1024ULL * 1024ULL ||
        (cluster_size & (cluster_size - 1U)) != 0U) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"縮小復元VDS FormatEx引数",
          L"ファイルシステムまたはクラスター寸法が未対応です");
    }
    if (clonecore::disk_operation_cancellation_requested(
            request_.callbacks)) {
      return status_failure(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小復元VDS FormatEx開始前取消",
          L"format開始前の安全な境界で取り消しました");
    }
    // VDS FormatEx is intentionally non-cancellable once submitted.  The
    // temporary GPT is retired only after its asynchronous HRESULT and the
    // filesystem/cluster readback are both verified.
    return format_exact_volume_with_vds(volume, file_system, cluster_size);
  }

  clonecore::Result<WindowsTsumugiShrinkFileSystemReadbackEvidence>
  verify_volume_readback(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      const imageformat::TsumugiManifestFileSystem file_system,
      const std::uint64_t cluster_size,
      const bool require_complete_applied_content_readback) override {
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return clonecore::Result<
          WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
          exact.error());
    }
    std::array<wchar_t, 32U> name{};
    if (!GetVolumeInformationW(
            volume.volume_device_path.c_str(),
            nullptr,
            0U,
            nullptr,
            nullptr,
            nullptr,
            name.data(),
            static_cast<DWORD>(name.size()))) {
      return clonecore::Result<
          WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::verification_failed,
              L"縮小復元ファイルシステム読戻し",
              GetLastError()));
    }
    const wchar_t* expected =
        file_system == imageformat::TsumugiManifestFileSystem::ntfs
        ? L"NTFS"
        : file_system == imageformat::TsumugiManifestFileSystem::exfat
              ? L"exFAT"
              : L"FAT32";
    DWORD sectors_per_cluster = 0U;
    DWORD bytes_per_sector = 0U;
    DWORD free_clusters = 0U;
    DWORD total_clusters = 0U;
    if (_wcsicmp(name.data(), expected) != 0 ||
        !GetDiskFreeSpaceW(
            volume.volume_device_path.c_str(),
            &sectors_per_cluster,
            &bytes_per_sector,
            &free_clusters,
            &total_clusters) ||
        sectors_per_cluster == 0U || bytes_per_sector == 0U ||
        static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector !=
            cluster_size) {
      return failure<WindowsTsumugiShrinkFileSystemReadbackEvidence>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"縮小復元ファイルシステム形式読戻し",
          L"形式またはクラスター寸法がレビュー済み値と一致しません");
    }
    WindowsTsumugiShrinkFileSystemReadbackEvidence evidence{
        .file_system_metadata_verified = true,
    };
    if (!require_complete_applied_content_readback) {
      return clonecore::Result<
          WindowsTsumugiShrinkFileSystemReadbackEvidence>::success(evidence);
    }

    auto privilege = enable_process_privilege(
        SE_BACKUP_NAME, L"縮小復元全通常ファイルBackup privilege");
    if (!privilege) {
      return clonecore::Result<
          WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
          privilege.error());
    }
    std::queue<std::wstring> pending;
    pending.push(volume.volume_device_path);
    evidence.directory_count = 1U;
    while (!pending.empty()) {
      if (clonecore::disk_operation_cancellation_requested(
              request_.callbacks)) {
        return failure<WindowsTsumugiShrinkFileSystemReadbackEvidence>(
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED,
            L"縮小復元namespace完全読戻し取消",
            L"ディレクトリ境界で安全に取り消しました");
      }
      auto directory = std::move(pending.front());
      pending.pop();
      std::wstring pattern = directory;
      if (!pattern.ends_with(L'\\')) {
        pattern.push_back(L'\\');
      }
      pattern.push_back(L'*');
      WIN32_FIND_DATAW data{};
      const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
      if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
          continue;
        }
        return clonecore::Result<
            WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::verification_failed,
                L"縮小復元namespace列挙開始",
                error));
      }
      std::vector<std::pair<std::wstring, DWORD>> children;
      do {
        const std::wstring_view child(data.cFileName);
        if (child == L"." || child == L"..") {
          continue;
        }
        std::wstring child_path = directory;
        if (!child_path.ends_with(L'\\')) {
          child_path.push_back(L'\\');
        }
        child_path.append(child);
        children.emplace_back(std::move(child_path), data.dwFileAttributes);
      } while (FindNextFileW(search, &data));
      const DWORD enumeration_error = GetLastError();
      const BOOL closed = FindClose(search);
      const DWORD close_error = GetLastError();
      if (enumeration_error != ERROR_NO_MORE_FILES || closed == FALSE) {
        return clonecore::Result<
            WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::verification_failed,
                L"縮小復元namespace完全列挙",
                enumeration_error != ERROR_NO_MORE_FILES
                    ? enumeration_error
                    : close_error));
      }
      for (auto& [child_path, attributes] : children) {
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
          if (evidence.reparse_point_count ==
              (std::numeric_limits<std::uint64_t>::max)()) {
            return failure<WindowsTsumugiShrinkFileSystemReadbackEvidence>(
                clonecore::ErrorCode::verification_failed,
                ERROR_ARITHMETIC_OVERFLOW,
                L"縮小復元reparse件数",
                L"読戻し証跡件数が表現範囲を超えました");
          }
          ++evidence.reparse_point_count;
          continue;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
          if (evidence.directory_count ==
              (std::numeric_limits<std::uint64_t>::max)()) {
            return failure<WindowsTsumugiShrinkFileSystemReadbackEvidence>(
                clonecore::ErrorCode::verification_failed,
                ERROR_ARITHMETIC_OVERFLOW,
                L"縮小復元directory件数",
                L"読戻し証跡件数が表現範囲を超えました");
          }
          ++evidence.directory_count;
          pending.push(std::move(child_path));
          continue;
        }
        auto bytes = read_regular_file_to_stable_eof(
            child_path, request_.callbacks);
        std::uint64_t total{};
        if (!bytes ||
            evidence.regular_file_count ==
                (std::numeric_limits<std::uint64_t>::max)() ||
            !checked_add(
                evidence.regular_file_bytes_read,
                bytes ? bytes.value() : 0U,
                total)) {
          return !bytes
              ? clonecore::Result<
                    WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
                    bytes.error())
              : failure<WindowsTsumugiShrinkFileSystemReadbackEvidence>(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_ARITHMETIC_OVERFLOW,
                    L"縮小復元全通常ファイル読戻し証跡",
                    L"件数または総読戻しbytesが表現範囲を超えました");
        }
        ++evidence.regular_file_count;
        evidence.regular_file_bytes_read = total;
      }
    }
    exact = validate_exact_binding(volume);
    if (!exact) {
      return clonecore::Result<
          WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
          exact.error());
    }
    evidence.namespace_fully_enumerated = true;
    evidence.every_regular_file_read_to_eof = true;
    return clonecore::Result<
        WindowsTsumugiShrinkFileSystemReadbackEvidence>::success(evidence);
  }

  clonecore::Result<WindowsTsumugiShrinkNtfsExtensionEvidence>
  extend_ntfs_volume_to_exact_extent_and_verify(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      const std::uint64_t previous_partition_size,
      const std::uint64_t cluster_size) override {
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return clonecore::Result<
          WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(exact.error());
    }
    if (previous_partition_size == 0U ||
        previous_partition_size >= volume.target_size ||
        previous_partition_size % 512U != 0U ||
        volume.target_size % 512U != 0U || cluster_size < 512U ||
        cluster_size > 2ULL * 1024ULL * 1024ULL ||
        (cluster_size & (cluster_size - 1U)) != 0U) {
      return failure<WindowsTsumugiShrinkNtfsExtensionEvidence>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"縮小復元NTFS exact伸長引数",
          L"旧・新partition寸法、512-byte境界、またはcluster寸法が未対応です");
    }
    if (clonecore::disk_operation_cancellation_requested(
            request_.callbacks)) {
      return failure<WindowsTsumugiShrinkNtfsExtensionEvidence>(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小復元NTFS exact伸長開始前取消",
          L"FSCTL_EXTEND_VOLUME開始前の安全な境界で取り消しました");
    }

    const std::wstring open_path = trim_volume_slash(volume.volume_device_path);
    clonecore::UniqueHandle handle(CreateFileW(
        open_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle) {
      return clonecore::Result<
          WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::access_denied,
              L"縮小復元NTFS exact伸長open",
              GetLastError()));
    }
    auto before = query_ntfs_volume_bytes(
        handle.get(), cluster_size, L"縮小復元NTFS伸長前sector読戻し");
    if (!before || before.value() != previous_partition_size) {
      return before
          ? failure<WindowsTsumugiShrinkNtfsExtensionEvidence>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"縮小復元NTFS伸長前寸法",
                L"NTFSがレビュー済みconstruction partition寸法と一致しません")
          : clonecore::Result<
                WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(
                before.error());
    }

    const std::uint64_t requested_sector_count = volume.target_size / 512U;
    if (requested_sector_count >
        static_cast<std::uint64_t>(
            (std::numeric_limits<LONGLONG>::max)())) {
      return failure<WindowsTsumugiShrinkNtfsExtensionEvidence>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小復元NTFS exact伸長sector数",
          L"最終partition寸法をFSCTL sector数で安全に表現できません");
    }
    LONGLONG requested_sectors =
        static_cast<LONGLONG>(requested_sector_count);
    DWORD returned = 0U;
    if (!DeviceIoControl(
            handle.get(),
            FSCTL_EXTEND_VOLUME,
            &requested_sectors,
            sizeof(requested_sectors),
            nullptr,
            0U,
            &returned,
            nullptr)) {
      return clonecore::Result<
          WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"縮小復元NTFS exact伸長control",
              GetLastError()));
    }
    if (!FlushFileBuffers(handle.get())) {
      return clonecore::Result<
          WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"縮小復元NTFS伸長flush",
              GetLastError()));
    }
    auto after = query_ntfs_volume_bytes(
        handle.get(), cluster_size, L"縮小復元NTFS伸長後sector読戻し");
    exact = validate_exact_binding(volume);
    if (!after || !exact || after.value() != volume.target_size) {
      return !after
          ? clonecore::Result<
                WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(
                after.error())
          : !exact
                ? clonecore::Result<
                      WindowsTsumugiShrinkNtfsExtensionEvidence>::failure(
                      exact.error())
                : failure<WindowsTsumugiShrinkNtfsExtensionEvidence>(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_CRC,
                      L"縮小復元NTFS伸長後寸法",
                      L"NTFS sector数が最終partition extent全体と一致しません");
    }
    return clonecore::Result<
        WindowsTsumugiShrinkNtfsExtensionEvidence>::success({
        .previous_file_system_bytes = before.value(),
        .final_file_system_bytes = after.value(),
        .final_partition_extent_bytes = volume.target_size,
        .exact_single_extent_reverified = true,
        .ntfs_sector_count_reverified = true,
        .flushed = true,
    });
  }

  clonecore::Status dismount_and_offline_volume(
      const WindowsTsumugiShrinkVolumeBinding& volume) override {
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return exact;
    }
    const std::wstring open_path = trim_volume_slash(volume.volume_device_path);
    clonecore::UniqueHandle handle(CreateFileW(
        open_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::access_denied,
          L"縮小復元Volume lock open",
          GetLastError()));
    }
    DWORD returned = 0U;
    if (!DeviceIoControl(
            handle.get(),
            FSCTL_LOCK_VOLUME,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) ||
        !DeviceIoControl(
            handle.get(),
            FSCTL_DISMOUNT_VOLUME,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) ||
        !DeviceIoControl(
            handle.get(),
            IOCTL_VOLUME_OFFLINE,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小復元Volume dismount/offline",
          GetLastError()));
    }
    return set_target_offline(true);
  }

  clonecore::Status begin_owned_staged_wim(
      const std::wstring& scratch_directory,
      const std::uint32_t source_table_index,
      const std::uint64_t expected_length) override {
    if (staged_handle_ || owns_staged_ || expected_length == 0U ||
        expected_length > static_cast<std::uint64_t>(
                              (std::numeric_limits<LONGLONG>::max)())) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"縮小復元所有WIM開始",
          L"WIM staging状態または予定長が不正です");
    }
    const DWORD scratch_attributes =
        GetFileAttributesW(scratch_directory.c_str());
    if (scratch_attributes == INVALID_FILE_ATTRIBUTES ||
        (scratch_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (scratch_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          L"縮小復元scratch検証",
          L"scratchは既存の通常ディレクトリでなければなりません");
    }
    auto path = make_staging_path(scratch_directory, source_table_index);
    auto descriptor = restricted_security_descriptor();
    if (!path || !descriptor) {
      return clonecore::Status::failure(
          !path ? path.error() : descriptor.error());
    }
    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.value().get(),
        .bInheritHandle = FALSE,
    };
    clonecore::UniqueHandle handle(CreateFileW(
        path.value().c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小復元所有WIM新規作成",
          GetLastError()));
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(expected_length);
    if (!SetFilePointerEx(handle.get(), end, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle.get())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小復元所有WIM予定長設定",
          GetLastError()));
    }
    staged_handle_ = std::move(handle);
    staged_path_ = path.take_value();
    staged_length_ = expected_length;
    staged_delivered_ = 0U;
    staged_source_table_index_ = source_table_index;
    owns_staged_ = true;
    staged_locked_ = false;
    staged_hash_.reset();
    return verify_regular_single_link(
        staged_handle_.get(), staged_length_, L"縮小復元所有WIM作成検証");
  }

  clonecore::Status append_owned_staged_wim(
      const std::uint64_t archive_offset,
      const std::uint64_t length,
      const bool zero_fill,
      const std::span<const std::byte> plaintext) override {
    std::uint64_t end{};
    if (!staged_handle_ || !owns_staged_ || staged_locked_ || length == 0U ||
        archive_offset != staged_delivered_ ||
        !checked_add(archive_offset, length, end) || end > staged_length_ ||
        (zero_fill ? !plaintext.empty() : plaintext.size() != length)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"縮小復元所有WIM追記",
          L"WIM chunkの順序、長さ、またはzero-fill表現が不正です");
    }
    if (clonecore::disk_operation_cancellation_requested(
            request_.callbacks)) {
      return status_failure(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小復元所有WIM追記取消",
          L"WIM chunk境界で安全に取り消しました");
    }
    if (!zero_fill) {
      auto status = write_handle_exact(
          staged_handle_.get(), archive_offset, plaintext);
      if (!status) {
        return status;
      }
    } else {
      const std::vector<std::byte> zeroes(kIoBlockBytes, std::byte{0});
      std::uint64_t offset = archive_offset;
      std::uint64_t remaining = length;
      while (remaining != 0U) {
        if (clonecore::disk_operation_cancellation_requested(
                request_.callbacks)) {
          return status_failure(
              clonecore::ErrorCode::cancelled,
              ERROR_CANCELLED,
              L"縮小復元所有WIM zero-fill取消",
              L"WIM zero-fill block境界で安全に取り消しました");
        }
        const auto block = static_cast<std::size_t>(
            (std::min<std::uint64_t>)(remaining, zeroes.size()));
        auto status = write_handle_exact(
            staged_handle_.get(), offset, std::span(zeroes).first(block));
        if (!status) {
          return status;
        }
        offset += block;
        remaining -= block;
      }
    }
    staged_delivered_ = end;
    return clonecore::success_status();
  }

  clonecore::Result<imageformat::Sha256Digest>
  verify_and_lock_single_image_wim(
      const std::uint32_t source_table_index) override {
    if (!staged_handle_ || !owns_staged_ || staged_locked_ ||
        source_table_index != staged_source_table_index_ ||
        staged_delivered_ != staged_length_ ||
        !FlushFileBuffers(staged_handle_.get())) {
      return failure<imageformat::Sha256Digest>(
          clonecore::ErrorCode::verification_failed,
          GetLastError(),
          L"縮小復元所有WIM完全性",
          L"全bytes、flush、またはsource bindingを確認できません");
    }
    auto regular = verify_regular_single_link(
        staged_handle_.get(), staged_length_, L"縮小復元所有WIM固定検証");
    auto digest = hash_file_handle(
        staged_handle_.get(), staged_length_, request_.callbacks);
    auto system = system_directory();
    if (!regular || !digest || !system) {
      return clonecore::Result<imageformat::Sha256Digest>::failure(
          !regular ? regular.error()
                   : !digest ? digest.error() : system.error());
    }
    auto wim = verify_wim_image_count(staged_path_, system.value(), *trust_);
    if (!wim) {
      return clonecore::Result<imageformat::Sha256Digest>::failure(wim.error());
    }
    staged_hash_ = digest.value();
    staged_locked_ = true;
    return digest;
  }

  clonecore::Status apply_locked_wim(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      const std::wstring& scratch_directory,
      const imageformat::Sha256Digest& locked_hash) override {
    if (!staged_handle_ || !owns_staged_ || !staged_locked_ ||
        !staged_hash_.has_value() || *staged_hash_ != locked_hash) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"縮小復元所有WIM適用前拘束",
          L"固定済みWIM hashまたはhandleが一致しません");
    }
    if (clonecore::disk_operation_cancellation_requested(
            request_.callbacks)) {
      return status_failure(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小復元DISM開始前取消",
          L"DISM適用開始前の安全な境界で取り消しました");
    }
    auto regular = verify_regular_single_link(
        staged_handle_.get(), staged_length_, L"縮小復元WIM適用前再確認");
    auto before = hash_file_handle(
        staged_handle_.get(), staged_length_, request_.callbacks);
    auto system = system_directory();
    if (!regular || !before || !system || before.value() != locked_hash) {
      return !regular
          ? regular
          : !before
                ? clonecore::Status::failure(before.error())
                : !system
                      ? clonecore::Status::failure(system.error())
                      : status_failure(
                            clonecore::ErrorCode::verification_failed,
                            ERROR_CRC,
                            L"縮小復元WIM適用前Hash",
                            L"固定後にWIM内容が変化しました");
    }
    auto exact = validate_exact_binding(volume);
    if (!exact) {
      return exact;
    }
    auto applied = windowsdism::execute_dism_apply(
        windowsdism::DismApplyRequest{
            .image_path = staged_path_,
            .target_root = volume.volume_device_path,
            .scratch_directory = scratch_directory,
        },
        system.value(),
        *trust_,
        *process_);
    if (!applied) {
      return clonecore::Status::failure(applied.error());
    }
    auto after = hash_file_handle(
        staged_handle_.get(), staged_length_, request_.callbacks);
    if (!after || after.value() != locked_hash) {
      return after
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"縮小復元WIM適用後Hash",
                L"DISM適用中に固定WIM内容が変化しました")
          : clonecore::Status::failure(after.error());
    }
    return validate_exact_binding(volume);
  }

  clonecore::Status discard_owned_staged_wim() override {
    if (!owns_staged_) {
      return clonecore::success_status();
    }
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    if (!staged_handle_ || !SetFileInformationByHandle(
                               staged_handle_.get(),
                               FileDispositionInfo,
                               &disposition,
                               sizeof(disposition))) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小復元所有WIM handle破棄",
          GetLastError()));
    }
    staged_handle_.reset();
    owns_staged_ = false;
    staged_path_.clear();
    staged_length_ = 0U;
    staged_delivered_ = 0U;
    staged_source_table_index_ = 0U;
    staged_locked_ = false;
    staged_hash_.reset();
    return clonecore::success_status();
  }

 private:
  WindowsTsumugiShrinkRestorePlatformRequest request_;
  std::unique_ptr<bootrepair::IExecutableTrustVerifier> trust_;
  std::unique_ptr<bootrepair::IProcessRunner> process_;
  clonecore::UniqueHandle staged_handle_;
  std::wstring staged_path_;
  std::uint64_t staged_length_{};
  std::uint64_t staged_delivered_{};
  std::uint32_t staged_source_table_index_{};
  std::optional<imageformat::Sha256Digest> staged_hash_;
  bool owns_staged_{};
  bool staged_locked_{};
};

class WindowsShrinkRestorePlatform final
    : public IWindowsTsumugiShrinkRestorePlatform {
 public:
  WindowsShrinkRestorePlatform(
      WindowsTsumugiShrinkRestorePlatformRequest request,
      std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo> io)
      : request_(std::move(request)), io_(std::move(io)) {}

  ~WindowsShrinkRestorePlatform() override {
    if (state_ == State::begun ||
        (state_ == State::aborted && !abort_cleanup_complete_)) {
      abort_keep_offline_incomplete();
    }
  }

  clonecore::Result<imageformat::TsumugiRestoreDiskIdentity>
  begin_offline_incomplete(
      const imageformat::TsumugiVerifiedImage& image,
      const imageformat::TsumugiWholeDiskRestoreTarget& target,
      const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
          reviewed_layout,
      const WindowsShrinkWorkPaths& work_paths) override {
    if (state_ != State::ready ||
        image.manifest.mode != imageformat::TsumugiManifestMode::shrink ||
        target.disk.is_running_windows_system_disk ||
        target.disk.is_usb_memory || target.disk.is_active_rescue_media ||
        target.disk.is_dynamic_disk || target.disk.is_storage_spaces ||
        target.disk.is_windows_software_raid ||
        target.disk.has_unresolved_hardware_raid ||
        reviewed_layout.metadata.target_size_bytes != target.disk.disk_size ||
        reviewed_layout.metadata.logical_sector_size !=
            target.disk.logical_sector_size ||
        reviewed_layout.migration.target_size_bytes != target.disk.disk_size) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小復元platform開始条件",
          L"縮小画像、非システム対象、またはレビュー済み寸法が不正です");
    }
    state_ = State::begun;
    auto observed = io_->observe_original_target(
        target.disk.connection_instance_hash);
    if (!observed ||
        !same_restore_identity(target.disk, observed.value().restore_identity)) {
      return observed
          ? failure<imageformat::TsumugiRestoreDiskIdentity>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_NOT_CONNECTED,
                L"Windows縮小復元先再識別",
                L"レビュー済み対象と現在の安定識別が一致しません")
          : clonecore::Result<
                imageformat::TsumugiRestoreDiskIdentity>::failure(
                observed.error());
    }
    target_identity_ = observed.value().restore_identity;
    auto status = io_->validate_work_paths_disjoint(
        work_paths, observed.value().physical.target.disk_number);
    if (!status) {
      return clonecore::Result<
          imageformat::TsumugiRestoreDiskIdentity>::failure(status.error());
    }
    auto model_hash = imageformat::hash_tsumugi_source_model_v1(
        observed.value().physical.target.model);
    auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
        observed.value().physical.target.serial_suffix,
        observed.value().physical.target.device_instance_id);
    if (!model_hash || !serial_hash) {
      return clonecore::Result<
          imageformat::TsumugiRestoreDiskIdentity>::failure(
          !model_hash ? model_hash.error() : serial_hash.error());
    }
    if (model_hash.value() == image.manifest.source_model_hash &&
        serial_hash.value() == image.manifest.source_serial_hash) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DRIVE,
          L"Windows縮小復元元ディスク保護",
          L"イメージを作成した元ディスク自身には復元できません");
    }
    target_touched_ = true;
    status = io_->set_target_offline(true);
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Result<
          imageformat::TsumugiRestoreDiskIdentity>::failure(error);
    }
    auto opened = io_->open_offline_target();
    if (!opened) {
      const auto error = opened.error();
      abort_keep_offline_incomplete();
      return clonecore::Result<
          imageformat::TsumugiRestoreDiskIdentity>::failure(error);
    }
    const auto stable = clonecore::validate_stable_identity(
        observed.value().physical.target_identity,
        opened.value().observed.target_identity,
        L"Windows縮小復元ロック済み対象");
    if (!stable || !opened.value().target ||
        opened.value().target->size_bytes() != target.disk.disk_size ||
        opened.value().target->logical_sector_size() !=
            target.disk.logical_sector_size ||
        !opened.value().observed.target.offline.value_or(false)) {
      auto result = failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Windows縮小復元対象handle照合",
          L"ロック済みWriterの識別、offline、容量、またはセクターが一致しません");
      abort_keep_offline_incomplete();
      return result;
    }
    writer_ = std::move(opened.value().target);
    auto generator = clonecore::make_windows_guid_generator();
    auto constructions =
        imageformat::make_tsumugi_shrink_construction_layout_plans_v1(
            reviewed_layout, *generator);
    if (!constructions) {
      const auto error = constructions.error();
      abort_keep_offline_incomplete();
      return clonecore::Result<
          imageformat::TsumugiRestoreDiskIdentity>::failure(
          error);
    }
    reviewed_layout_ = reviewed_layout;
    construction_plans_ = constructions.value();
    layout_ = std::make_unique<
        imageformat::TsumugiShrinkRestoreLayoutTransactionV1>(
        reviewed_layout, constructions.take_value(), *writer_);
    auto prepared = layout_->prepare(request_.callbacks);
    if (!prepared) {
      const auto error = prepared.error();
      abort_keep_offline_incomplete();
      return clonecore::Result<
          imageformat::TsumugiRestoreDiskIdentity>::failure(error);
    }
    for (const auto& partition : reviewed_layout.migration.target_partitions) {
      if (partition.action ==
          migrationcore::MigrationPartitionAction::copy_exact_raw) {
        exact_expected_.emplace(partition.target_number, partition.size_bytes);
        exact_delivered_.emplace(partition.target_number, 0U);
      }
    }
    work_paths_ = work_paths;
    for (const auto& construction : construction_plans_) {
      if (construction.purpose !=
          imageformat::TsumugiShrinkConstructionPurposeV1::
              prepare_efi_system) {
        continue;
      }
      status = start_construction(
          construction,
          imageformat::TsumugiManifestFileSystem::fat32,
          4096U);
      if (!status) {
        const auto error = status.error();
        abort_keep_offline_incomplete();
        return clonecore::Result<
            imageformat::TsumugiRestoreDiskIdentity>::failure(error);
      }
      status = finish_construction(false);
      if (!status) {
        const auto error = status.error();
        abort_keep_offline_incomplete();
        return clonecore::Result<
            imageformat::TsumugiRestoreDiskIdentity>::failure(error);
      }
    }
    return clonecore::Result<
        imageformat::TsumugiRestoreDiskIdentity>::success(target_identity_);
  }

  clonecore::Status create_target_file_system(
      const imageformat::TsumugiShrinkArchiveTarget& target) override {
    const auto* construction = construction_for_archive(target);
    if (state_ != State::begun || active_construction_number_.has_value() ||
        construction == nullptr || !valid_file_system(target.file_system) ||
        target.cluster_size == 0U) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小復元FS作成対応",
          L"archiveとレビュー済み一時GPTの対応が一意ではありません");
    }
    active_archive_ = target;
    const auto status = start_construction(
        *construction, target.file_system, target.cluster_size);
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    return clonecore::success_status();
  }

  clonecore::Status begin_staged_wim(
      const imageformat::TsumugiShrinkArchiveTarget& target,
      const std::wstring& scratch_directory) override {
    if (!active_archive_.has_value() || !active_volume_.has_value() ||
        staged_started_ ||
        active_archive_->source_table_index != target.source_table_index ||
        active_archive_->target_offset != target.target_offset ||
        active_archive_->target_size != target.target_size ||
        scratch_directory != work_paths_.scratch_directory) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小復元WIM staging対応",
          L"WIM、scratch、または現在の一時Volumeが一致しません");
    }
    auto status = io_->begin_owned_staged_wim(
        scratch_directory,
        target.source_table_index,
        target.archive_length);
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    staged_started_ = true;
    staged_delivered_ = 0U;
    return clonecore::success_status();
  }

  clonecore::Status append_staged_wim(
      const imageformat::TsumugiShrinkArchiveChunk& chunk,
      const std::span<const std::byte> plaintext) override {
    std::uint64_t next{};
    if (!staged_started_ || !active_archive_.has_value() ||
        chunk.source_table_index != active_archive_->source_table_index ||
        chunk.archive_offset != staged_delivered_ || chunk.length == 0U ||
        !checked_add(staged_delivered_, chunk.length, next) ||
        next > active_archive_->archive_length) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小復元WIM staging順序",
          L"WIM chunkの順序または範囲が一致しません");
    }
    auto status = io_->append_owned_staged_wim(
        chunk.archive_offset,
        chunk.length,
        chunk.zero_fill,
        plaintext);
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    staged_delivered_ = next;
    return clonecore::success_status();
  }

  clonecore::Status verify_staged_single_image_wim(
      const std::uint32_t source_table_index) override {
    if (!staged_started_ || staged_verified_ || !active_archive_.has_value() ||
        source_table_index != active_archive_->source_table_index ||
        staged_delivered_ != active_archive_->archive_length) {
      return fail_and_abort(
          clonecore::ErrorCode::verification_failed,
          ERROR_HANDLE_EOF,
          L"Windows縮小復元WIM完全性",
          L"全WIM bytesが到着していないか、検証状態が不正です");
    }
    auto verified = io_->verify_and_lock_single_image_wim(source_table_index);
    if (!verified) {
      const auto error = verified.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    staged_hash_ = verified.value();
    staged_verified_ = true;
    return clonecore::success_status();
  }

  clonecore::Status apply_staged_wim(
      const std::uint32_t source_table_index) override {
    if (!staged_verified_ || wim_applied_ || !staged_hash_.has_value() ||
        !active_archive_.has_value() || !active_volume_.has_value() ||
        active_archive_->source_table_index != source_table_index) {
      return fail_and_abort(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Windows縮小復元WIM適用状態",
          L"固定検証済みWIMと一時Volumeが必要です");
    }
    auto status = io_->apply_locked_wim(
        *active_volume_, work_paths_.scratch_directory, *staged_hash_);
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    wim_applied_ = true;
    return clonecore::success_status();
  }

  clonecore::Status verify_applied_file_system_readback(
      const imageformat::TsumugiShrinkArchiveTarget& target) override {
    if (!active_archive_.has_value() || !active_volume_.has_value() ||
        active_archive_->source_table_index != target.source_table_index ||
        active_archive_->target_offset != target.target_offset ||
        active_archive_->target_size != target.target_size ||
        (staged_started_ && !wim_applied_)) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_STATE,
          L"Windows縮小復元FS読戻し対応",
          L"適用対象と現在の一時Volumeが一致しません");
    }
    auto status = finish_construction(staged_started_);
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    return clonecore::success_status();
  }

  clonecore::Status write_exact_raw_and_verify(
      const imageformat::TsumugiRestoreWrite& write,
      const std::span<const std::byte> plaintext) override {
    if (state_ != State::begun || active_construction_number_.has_value() ||
        disk_online_ || !writer_ ||
        write.stable_target_identity_hash !=
            target_identity_.stable_identity_hash ||
        write.length == 0U ||
        write.length % writer_->logical_sector_size() != 0U ||
        write.target_offset % writer_->logical_sector_size() != 0U ||
        (write.zero_fill ? !plaintext.empty()
                         : plaintext.size() != write.length)) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小復元exact RAW状態",
          L"対象、整列、長さ、またはoffline状態が不正です");
    }
    if (clonecore::disk_operation_cancellation_requested(
            request_.callbacks)) {
      return fail_and_abort(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"Windows縮小復元exact RAW取消",
          L"RAW chunk開始前の安全な境界で取り消しました");
    }
    const auto partition = std::find_if(
        reviewed_layout_.migration.target_partitions.begin(),
        reviewed_layout_.migration.target_partitions.end(),
        [&](const auto& candidate) {
          return candidate.source_table_index == write.source_table_index &&
              candidate.action ==
                  migrationcore::MigrationPartitionAction::copy_exact_raw;
        });
    if (partition == reviewed_layout_.migration.target_partitions.end()) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Windows縮小復元exact RAW binding",
          L"レビュー済みRAWパーティションがありません");
    }
    auto& delivered = exact_delivered_.at(partition->target_number);
    std::uint64_t expected_offset{};
    std::uint64_t next{};
    if (!checked_add(partition->offset_bytes, delivered, expected_offset) ||
        expected_offset != write.target_offset ||
        !checked_add(delivered, write.length, next) ||
        next > exact_expected_.at(partition->target_number)) {
      return fail_and_abort(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小復元exact RAW順序",
          L"RAW writeがレビュー済みパーティション範囲と一致しません");
    }
    const auto write_block = [&](const std::uint64_t offset,
                                 const std::span<const std::byte> bytes) {
      auto status = writer_->write_target(offset, bytes);
      if (!status) {
        return status;
      }
      status = writer_->flush_target();
      if (!status) {
        return status;
      }
      auto read = writer_->read_back(offset, bytes.size());
      if (!read) {
        return clonecore::Status::failure(read.error());
      }
      return read.value().size() == bytes.size() &&
              std::equal(bytes.begin(), bytes.end(), read.value().begin())
          ? clonecore::success_status()
          : status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Windows縮小復元exact RAW読戻し",
                L"同じ対象handleからの読戻しが一致しません");
    };
    clonecore::Status status = clonecore::success_status();
    if (!write.zero_fill) {
      status = write_block(write.target_offset, plaintext);
    } else {
      const std::vector<std::byte> zeroes(kIoBlockBytes, std::byte{0});
      std::uint64_t offset = write.target_offset;
      std::uint64_t remaining = write.length;
      while (remaining != 0U && status) {
        if (clonecore::disk_operation_cancellation_requested(
                request_.callbacks)) {
          status = status_failure(
              clonecore::ErrorCode::cancelled,
              ERROR_CANCELLED,
              L"Windows縮小復元exact RAW zero-fill取消",
              L"zero-fill block境界で安全に取り消しました");
          break;
        }
        const auto block = static_cast<std::size_t>(
            (std::min<std::uint64_t>)(remaining, zeroes.size()));
        status = write_block(offset, std::span(zeroes).first(block));
        offset += block;
        remaining -= block;
      }
    }
    if (!status) {
      const auto error = status.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    delivered = next;
    return clonecore::success_status();
  }

  clonecore::Status commit_final_layout_last() override {
    const bool exact_complete = std::all_of(
        exact_expected_.begin(), exact_expected_.end(), [&](const auto& item) {
          const auto delivered = exact_delivered_.find(item.first);
          return delivered != exact_delivered_.end() &&
              delivered->second == item.second;
        });
    if (state_ != State::begun || !layout_ || disk_online_ ||
        active_construction_number_.has_value() || staged_started_ ||
        !exact_complete) {
      return fail_and_abort(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"Windows縮小復元最終commit条件",
          L"全一時GPT退役、WIM/RAW読戻し、対象offlineが完了していません");
    }
    auto offline = io_->set_target_offline(true);
    if (!offline) {
      const auto error = offline.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    auto committed = layout_->commit_final(request_.callbacks);
    if (!committed ||
        !committed.value().final_partition_table_committed ||
        !committed.value().every_temporary_write_read_back_verified) {
      const auto error = committed
          ? platform_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Windows縮小復元最終commit証跡",
                L"最終表または全一時metadata読戻し証跡が不足しています")
          : committed.error();
      abort_keep_offline_incomplete();
      return clonecore::Status::failure(error);
    }
    // The completed disk intentionally stays offline.  Do not issue a later
    // fallible mount-manager notification after the atomic metadata commit;
    // Windows refreshes the final table when the user explicitly onlines it.
    state_ = State::committed;
    return clonecore::success_status();
  }

  void abort_keep_offline_incomplete() noexcept override {
    if (state_ == State::committed || state_ == State::ready ||
        (state_ == State::aborted && abort_cleanup_complete_)) {
      return;
    }
    if (!target_touched_) {
      state_ = State::aborted;
      abort_cleanup_complete_ = true;
      return;
    }
    bool staged_discarded = false;
    try {
      staged_discarded = io_->discard_owned_staged_wim().has_value();
    } catch (...) {
      staged_discarded = false;
    }

    // Once an online attempt has begun, metadata retirement is forbidden
    // until either the exact bound volume has been dismounted/offlined or the
    // verified whole target has been offlined. A failed attempt retains the
    // binding and online state so a later idempotent abort retries the exact
    // dismount instead of erasing live volume metadata.
    bool safely_offline = !disk_online_;
    if (disk_online_ && active_volume_.has_value()) {
      try {
        const auto dismounted =
            io_->dismount_and_offline_volume(*active_volume_);
        if (dismounted) {
          disk_online_ = false;
          safely_offline = true;
        }
      } catch (...) {
      }
    }
    if (!safely_offline) {
      try {
        const auto offlined = io_->set_target_offline(true);
        if (offlined) {
          disk_online_ = false;
          safely_offline = true;
        }
      } catch (...) {
      }
    }

    bool metadata_safely_withheld = layout_ == nullptr;
    bool layout_notified = layout_ == nullptr;
    if (safely_offline && layout_) {
      try {
        layout_->abort();
        const auto& report = layout_->report();
        metadata_safely_withheld = report.metadata_safely_withheld &&
            report.target_left_incomplete &&
            !report.temporary_layout_active &&
            !report.final_partition_table_committed;
      } catch (...) {
        metadata_safely_withheld = false;
      }
      try {
        layout_notified = io_->notify_layout_changed().has_value();
      } catch (...) {
        layout_notified = false;
      }
    }

    bool target_offline_verified = false;
    if (safely_offline) {
      try {
        const auto offlined = io_->set_target_offline(true);
        target_offline_verified = offlined.has_value();
        if (offlined) {
          disk_online_ = false;
        }
      } catch (...) {
        target_offline_verified = false;
      }
    }

    if (staged_discarded) {
      staged_hash_.reset();
      staged_started_ = false;
      staged_verified_ = false;
      wim_applied_ = false;
      staged_delivered_ = 0U;
    }
    if (metadata_safely_withheld) {
      active_volume_.reset();
      active_archive_.reset();
      active_construction_number_.reset();
    }
    state_ = State::aborted;
    abort_cleanup_complete_ = staged_discarded && safely_offline &&
        metadata_safely_withheld && layout_notified &&
        target_offline_verified;
  }

 private:
  enum class State : std::uint8_t { ready, begun, committed, aborted };

  const imageformat::TsumugiShrinkConstructionLayoutPlanV1*
  construction_for_archive(
      const imageformat::TsumugiShrinkArchiveTarget& target) const noexcept {
    const auto found = std::find_if(
        construction_plans_.begin(), construction_plans_.end(),
        [&](const auto& plan) {
          return plan.source_table_index == target.source_table_index &&
              plan.target_offset == target.target_offset &&
              plan.target_size == target.target_size &&
              (plan.purpose ==
                   imageformat::TsumugiShrinkConstructionPurposeV1::
                       apply_file_image ||
               plan.purpose ==
                   imageformat::TsumugiShrinkConstructionPurposeV1::
                       recreate_empty_file_system);
        });
    return found == construction_plans_.end() ? nullptr : &*found;
  }

  clonecore::Status start_construction(
      const imageformat::TsumugiShrinkConstructionLayoutPlanV1& construction,
      const imageformat::TsumugiManifestFileSystem file_system,
      const std::uint64_t cluster_size) {
    if (!layout_ || active_construction_number_.has_value() || disk_online_) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Windows縮小復元一時GPT開始",
          L"同時に公開できる一時GPTは1件だけです");
    }
    auto published = layout_->publish_construction(
        construction.final_target_number, request_.callbacks);
    if (!published) {
      return clonecore::Status::failure(published.error());
    }
    active_construction_number_ = construction.final_target_number;
    auto status = io_->notify_layout_changed();
    if (!status) {
      return status;
    }
    // A failed online request can still have changed device state. Mark it as
    // potentially online before issuing the request so abort must prove an
    // offline transition before retiring the temporary GPT.
    disk_online_ = true;
    status = io_->set_target_offline(false);
    if (!status) {
      return status;
    }
    auto volume = io_->bind_online_volume(
        construction.final_target_number,
        construction.target_offset,
        construction.target_size);
    if (!volume) {
      return clonecore::Status::failure(volume.error());
    }
    active_volume_ = volume.take_value();
    status = io_->format_volume(*active_volume_, file_system, cluster_size);
    if (!status) {
      return status;
    }
    active_file_system_ = file_system;
    active_cluster_size_ = cluster_size;
    return clonecore::success_status();
  }

  clonecore::Status finish_construction(
      const bool require_complete_applied_content_readback) {
    if (!layout_ || !active_volume_.has_value() ||
        !active_construction_number_.has_value()) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Windows縮小復元一時GPT完了",
          L"完了対象の一時Volumeがありません");
    }
    auto readback = io_->verify_volume_readback(
        *active_volume_,
        active_file_system_,
        active_cluster_size_,
        require_complete_applied_content_readback);
    if (!readback) {
      return clonecore::Status::failure(readback.error());
    }
    const auto& evidence = readback.value();
    if (!evidence.file_system_metadata_verified ||
        (require_complete_applied_content_readback &&
         (!evidence.namespace_fully_enumerated ||
          !evidence.every_regular_file_read_to_eof ||
          evidence.regular_file_count == 0U))) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Windows縮小復元全filesystem読戻し証跡",
          L"全namespace列挙、全通常ファイルEOF読戻し、または件数証跡が不足しています");
    }
    auto status = clonecore::success_status();
    if (staged_started_) {
      status = io_->discard_owned_staged_wim();
      if (!status) {
        return status;
      }
    }
    status = io_->dismount_and_offline_volume(*active_volume_);
    if (!status) {
      return status;
    }
    disk_online_ = false;
    const auto final_target_number = *active_construction_number_;
    auto retired = layout_->retire_construction(
        final_target_number, request_.callbacks);
    if (!retired) {
      return clonecore::Status::failure(retired.error());
    }
    status = io_->notify_layout_changed();
    if (!status) {
      return status;
    }
    active_volume_.reset();
    active_archive_.reset();
    active_construction_number_.reset();
    staged_hash_.reset();
    staged_started_ = false;
    staged_verified_ = false;
    wim_applied_ = false;
    staged_delivered_ = 0U;
    return clonecore::success_status();
  }

  clonecore::Status fail_and_abort(
      const clonecore::ErrorCode code,
      const DWORD native_code,
      std::wstring operation,
      std::wstring message) {
    const auto error = platform_error(
        code, native_code, std::move(operation), std::move(message));
    abort_keep_offline_incomplete();
    return clonecore::Status::failure(error);
  }

  WindowsTsumugiShrinkRestorePlatformRequest request_;
  std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo> io_;
  State state_{State::ready};
  WindowsShrinkWorkPaths work_paths_;
  imageformat::TsumugiRestoreDiskIdentity target_identity_{};
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 reviewed_layout_;
  std::vector<imageformat::TsumugiShrinkConstructionLayoutPlanV1>
      construction_plans_;
  std::unique_ptr<clonecore::ITargetDiskWriter> writer_;
  std::unique_ptr<imageformat::TsumugiShrinkRestoreLayoutTransactionV1>
      layout_;
  std::optional<std::uint32_t> active_construction_number_;
  std::optional<WindowsTsumugiShrinkVolumeBinding> active_volume_;
  std::optional<imageformat::TsumugiShrinkArchiveTarget> active_archive_;
  imageformat::TsumugiManifestFileSystem active_file_system_{
      imageformat::TsumugiManifestFileSystem::unknown};
  std::uint64_t active_cluster_size_{};
  std::optional<imageformat::Sha256Digest> staged_hash_;
  std::uint64_t staged_delivered_{};
  std::map<std::uint32_t, std::uint64_t> exact_expected_;
  std::map<std::uint32_t, std::uint64_t> exact_delivered_;
  bool disk_online_{};
  bool staged_started_{};
  bool staged_verified_{};
  bool wim_applied_{};
  bool target_touched_{};
  bool abort_cleanup_complete_{};
};

clonecore::Status validate_request(
    const WindowsTsumugiShrinkRestorePlatformRequest& request) {
  const auto identity = clonecore::validate_stable_identity(
      request.expected_target, request.expected_target, L"Windows縮小復元先");
  if (!identity) {
    return identity;
  }
  if (!request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK" ||
      all_zero(request.expected_target_layout_hash) ||
      request.expected_target.is_system_disk) {
    return status_failure(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"Windows縮小復元platform要求",
        L"非システム対象、レイアウトHash、および大文字OK確認が必要です");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform>>
make_windows_tsumugi_shrink_restore_platform(
    const WindowsTsumugiShrinkRestorePlatformRequest& request) {
  auto io = make_windows_tsumugi_shrink_restore_platform_io(request);
  if (!io) {
    return clonecore::Result<std::unique_ptr<
        IWindowsTsumugiShrinkRestorePlatform>>::failure(io.error());
  }
  return make_windows_tsumugi_shrink_restore_platform_with_io(
      request, io.take_value());
}

clonecore::Result<std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo>>
make_windows_tsumugi_shrink_restore_platform_io(
    const WindowsTsumugiShrinkRestorePlatformRequest& request) {
  auto valid = validate_request(request);
  if (!valid) {
    return clonecore::Result<std::unique_ptr<
        IWindowsTsumugiShrinkRestorePlatformIo>>::failure(valid.error());
  }
  std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo> io =
      std::make_unique<Win32ShrinkRestoreIo>(request);
  return clonecore::Result<std::unique_ptr<
      IWindowsTsumugiShrinkRestorePlatformIo>>::success(std::move(io));
}

clonecore::Result<std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform>>
make_windows_tsumugi_shrink_restore_platform_with_io(
    const WindowsTsumugiShrinkRestorePlatformRequest& request,
    std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo> io) {
  auto valid = validate_request(request);
  if (!valid) {
    return clonecore::Result<std::unique_ptr<
        IWindowsTsumugiShrinkRestorePlatform>>::failure(valid.error());
  }
  if (!io) {
    return failure<std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_HANDLE,
        L"Windows縮小復元platform I/O",
        L"I/O実装がありません");
  }
  return clonecore::Result<std::unique_ptr<
      IWindowsTsumugiShrinkRestorePlatform>>::success(
      std::make_unique<WindowsShrinkRestorePlatform>(
          request, std::move(io)));
}

}  // namespace ytec::windowsapp

#ifdef _MSC_VER
// Windows SDK 10.0.26100 applies a value-incompatible SAL annotation to
// RegOpenKeyExW parameter 3. Suppress that SDK declaration false positive.
#pragma warning(disable : 6553)
#endif

#include "system_disk.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ytec::diskmodel {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::UniqueHandle;

constexpr std::size_t kMaximumVolumeExtentCount = 256;
constexpr std::size_t kMaximumVolumeExtentBytes = 64U * 1024U;

Error system_disk_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool is_winpe() {
  HKEY key = nullptr;
  const LSTATUS status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
      0,
      KEY_QUERY_VALUE,
      &key);
  if (key != nullptr) {
    RegCloseKey(key);
  }
  return status == ERROR_SUCCESS;
}

Result<std::wstring> current_system_volume_path() {
  std::vector<wchar_t> windows_directory(32U * 1024U, L'\0');
  const UINT length = GetWindowsDirectoryW(
      windows_directory.data(),
      static_cast<UINT>(windows_directory.size()));
  if (length == 0 || length >= windows_directory.size()) {
    return Result<std::wstring>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"現在のWindowsディレクトリ取得",
        GetLastError()));
  }

  std::vector<wchar_t> volume_root(32U * 1024U, L'\0');
  if (!GetVolumePathNameW(
          windows_directory.data(),
          volume_root.data(),
          static_cast<DWORD>(volume_root.size()))) {
    return Result<std::wstring>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"現在のWindowsボリューム取得",
        GetLastError()));
  }

  std::vector<wchar_t> volume_name(32U * 1024U, L'\0');
  if (GetVolumeNameForVolumeMountPointW(
          volume_root.data(),
          volume_name.data(),
          static_cast<DWORD>(volume_name.size()))) {
    std::wstring path(volume_name.data());
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
      path.pop_back();
    }
    if (!path.empty()) {
      return Result<std::wstring>::success(std::move(path));
    }
  }

  const std::wstring root(volume_root.data());
  if (root.size() >= 3 && root[1] == L':' &&
      (root[2] == L'\\' || root[2] == L'/')) {
    return Result<std::wstring>::success(
        L"\\\\.\\" + root.substr(0, 2));
  }
  return Result<std::wstring>::failure(system_disk_error(
      ErrorCode::query_failed,
      ERROR_INVALID_DRIVE,
      L"現在のWindowsボリュームパス検証",
      L"実行中Windowsのボリュームを安全に識別できません"));
}

}  // namespace

Result<std::vector<std::uint32_t>> query_windows_system_disk_numbers() {
  const auto volume_path = current_system_volume_path();
  if (!volume_path) {
    return Result<std::vector<std::uint32_t>>::failure(volume_path.error());
  }
  UniqueHandle volume(CreateFileW(
      volume_path.value().c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    if (is_winpe()) {
      return Result<std::vector<std::uint32_t>>::success({});
    }
    return Result<std::vector<std::uint32_t>>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"実行中Windowsボリュームのオープン",
            GetLastError()));
  }

  std::vector<std::byte> buffer(4096);
  DWORD bytes_returned = 0;
  for (;;) {
    if (DeviceIoControl(
            volume.get(),
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            nullptr,
            0,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned,
            nullptr)) {
      break;
    }
    const DWORD native_code = GetLastError();
    if ((native_code == ERROR_MORE_DATA ||
         native_code == ERROR_INSUFFICIENT_BUFFER) &&
        buffer.size() < kMaximumVolumeExtentBytes) {
      buffer.resize(std::min(
          buffer.size() * 2U, kMaximumVolumeExtentBytes));
      continue;
    }
    if (is_winpe() &&
        (native_code == ERROR_INVALID_FUNCTION ||
         native_code == ERROR_NOT_SUPPORTED)) {
      return Result<std::vector<std::uint32_t>>::success({});
    }
    return Result<std::vector<std::uint32_t>>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"実行中Windowsのディスク範囲取得",
            native_code));
  }

  constexpr std::size_t header_size =
      offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (bytes_returned < header_size) {
    return Result<std::vector<std::uint32_t>>::failure(system_disk_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"実行中Windowsのディスク範囲検証",
        L"ディスク範囲応答が短すぎます"));
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents == 0 ||
      extents->NumberOfDiskExtents > kMaximumVolumeExtentCount) {
    return Result<std::vector<std::uint32_t>>::failure(system_disk_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"実行中Windowsのディスク範囲検証",
        L"ディスク範囲件数が不正です"));
  }
  const std::size_t required = header_size +
      static_cast<std::size_t>(extents->NumberOfDiskExtents) *
          sizeof(DISK_EXTENT);
  if (required > bytes_returned) {
    return Result<std::vector<std::uint32_t>>::failure(system_disk_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"実行中Windowsのディスク範囲検証",
        L"ディスク範囲応答に全要素が含まれていません"));
  }

  std::vector<std::uint32_t> disk_numbers;
  disk_numbers.reserve(extents->NumberOfDiskExtents);
  for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index) {
    disk_numbers.push_back(extents->Extents[index].DiskNumber);
  }
  std::sort(disk_numbers.begin(), disk_numbers.end());
  disk_numbers.erase(
      std::unique(disk_numbers.begin(), disk_numbers.end()),
      disk_numbers.end());
  return Result<std::vector<std::uint32_t>>::success(std::move(disk_numbers));
}

}  // namespace ytec::diskmodel

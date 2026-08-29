#include "ytec/clonecore/log.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/winpeapp/active_rescue_media.h"
#include "ytec/winpeapp/app_runner.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef YTEC_WINPE_PRODUCT_BOUNDARY
#error WinPEApp must be built with the product safety boundary enabled.
#endif

namespace {

constexpr std::wstring_view kLaunchGuiFromMediaArgument =
    L"--launch-gui-from-media";
constexpr std::wstring_view kWinPEGuiRelativePath =
    L"YtecDiskClone\\ytec-winpe-gui.exe";
constexpr std::wstring_view kWinPEDataRelativePath = L"YtecDiskClone\\data";
constexpr std::size_t kRescueMediaMarkerBytes = 36U;

bool same_path_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      CompareStringOrdinal(
          left.data(),
          static_cast<int>(left.size()),
          right.data(),
          static_cast<int>(right.size()),
          TRUE) == CSTR_EQUAL;
}

std::wstring strip_extended_dos_prefix(std::wstring value) {
  constexpr std::wstring_view kPrefix{L"\\\\?\\"};
  if (value.starts_with(kPrefix)) {
    value.erase(0U, kPrefix.size());
  }
  return value;
}

bool verify_opened_path(
    const HANDLE handle,
    const std::wstring& expected_path) {
  const DWORD required = GetFinalPathNameByHandleW(
      handle, nullptr, 0U, VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
  if (required == 0U || required >= 32U * 1024U) {
    return false;
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
  const DWORD copied = GetFinalPathNameByHandleW(
      handle,
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      VOLUME_NAME_DOS | FILE_NAME_NORMALIZED);
  if (copied == 0U || copied >= buffer.size()) {
    return false;
  }
  std::wstring opened = strip_extended_dos_prefix(
      std::wstring(buffer.data(), copied));
  std::vector<wchar_t> full(32U * 1024U, L'\0');
  const DWORD full_length = GetFullPathNameW(
      expected_path.c_str(),
      static_cast<DWORD>(full.size()),
      full.data(),
      nullptr);
  return full_length > 0U && full_length < full.size() &&
      same_path_ordinal_ignore_case(
          opened, std::wstring_view(full.data(), full_length));
}

bool open_and_verify_normal_path(
    const std::wstring& path,
    const bool directory,
    ytec::clonecore::UniqueHandle& opened) {
  const DWORD expected_attributes = GetFileAttributesW(path.c_str());
  if (expected_attributes == INVALID_FILE_ATTRIBUTES ||
      (expected_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      ((expected_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) != directory) {
    return false;
  }
  ytec::clonecore::UniqueHandle candidate(CreateFileW(
      path.c_str(),
      directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
      directory ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
                : FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT |
          (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN),
      nullptr));
  if (!candidate) {
    return false;
  }
  DWORD opened_attributes{};
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileInformationByHandleEx(
          candidate.get(), FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE) {
    opened_attributes = tag.FileAttributes;
  } else if (GetLastError() == ERROR_INVALID_PARAMETER) {
    BY_HANDLE_FILE_INFORMATION basic{};
    if (GetFileInformationByHandle(candidate.get(), &basic) == FALSE) {
      return false;
    }
    opened_attributes = basic.dwFileAttributes;
  } else {
    return false;
  }
  if ((opened_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      ((opened_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) != directory ||
      (!directory && GetFileType(candidate.get()) != FILE_TYPE_DISK) ||
      !verify_opened_path(candidate.get(), path)) {
    return false;
  }
  opened = std::move(candidate);
  return true;
}

bool read_marker_from_held_handle(
    const std::wstring& path,
    ytec::clonecore::UniqueHandle& opened,
    std::array<char, kRescueMediaMarkerBytes>& bytes) {
  if (!open_and_verify_normal_path(path, false, opened)) {
    return false;
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(opened.get(), &size) == FALSE ||
      size.QuadPart != static_cast<LONGLONG>(bytes.size())) {
    return false;
  }
  DWORD read{};
  return ReadFile(
             opened.get(),
             bytes.data(),
             static_cast<DWORD>(bytes.size()),
             &read,
             nullptr) != FALSE &&
      read == bytes.size();
}

int launch_gui_from_verified_rescue_media() {
  auto storage =
      ytec::winpeapp::query_active_rescue_media_storage_with_windows_apis();
  if (!storage) {
    std::wcerr << L"レスキュー媒体を一意に確認できません: "
               << storage.error().operation << L" / "
               << storage.error().message << L'\n';
    return 2;
  }
  const auto& observed = storage.value();
  const bool supported_drive_type = observed.drive_type == DRIVE_CDROM ||
      observed.drive_type == DRIVE_FIXED ||
      observed.drive_type == DRIVE_REMOVABLE;
  if (!supported_drive_type ||
      !observed.marker_identity_from_open_handle ||
      (observed.drive_type == DRIVE_CDROM &&
       observed.physical_identity.has_value()) ||
      ((observed.drive_type == DRIVE_FIXED ||
        observed.drive_type == DRIVE_REMOVABLE) &&
       !observed.physical_identity.has_value())) {
    std::wcerr << L"レスキュー媒体のopened-handle識別結果が不整合です。\n";
    return 3;
  }

  ytec::clonecore::UniqueHandle runtime_marker_handle;
  ytec::clonecore::UniqueHandle media_marker_handle;
  std::array<char, kRescueMediaMarkerBytes> runtime_marker{};
  std::array<char, kRescueMediaMarkerBytes> media_marker{};
  if (!read_marker_from_held_handle(
          std::wstring(
              ytec::winpeapp::kActiveRescueMediaRuntimeMarkerPath),
          runtime_marker_handle,
          runtime_marker) ||
      !read_marker_from_held_handle(
          observed.marker_path, media_marker_handle, media_marker) ||
      runtime_marker != media_marker) {
    std::wcerr << L"起動WIMと媒体ルートのマーカーをopened handleで固定できません。\n";
    return 4;
  }
  auto fresh_storage =
      ytec::winpeapp::query_active_rescue_media_storage_with_windows_apis();
  if (!fresh_storage ||
      !same_path_ordinal_ignore_case(
          observed.marker_path, fresh_storage.value().marker_path) ||
      observed.drive_type != fresh_storage.value().drive_type ||
      !fresh_storage.value().marker_identity_from_open_handle ||
      observed.physical_identity.has_value() !=
          fresh_storage.value().physical_identity.has_value()) {
    std::wcerr << L"GUI起動直前にレスキュー媒体の識別結果が変化しました。\n";
    return 5;
  }
  if (observed.physical_identity.has_value()) {
    const auto identity = ytec::clonecore::validate_stable_identity(
        *observed.physical_identity,
        *fresh_storage.value().physical_identity,
        L"GUI起動媒体");
    if (!identity) {
      std::wcerr << L"GUI起動直前にレスキュー媒体の安定識別が変化しました。\n";
      return 6;
    }
  }

  const std::filesystem::path marker_path(observed.marker_path);
  const std::wstring media_root = marker_path.root_path().wstring();
  const std::wstring executable = observed.drive_type == DRIVE_CDROM
      ? L"X:\\" + std::wstring(kWinPEGuiRelativePath)
      : media_root + std::wstring(kWinPEGuiRelativePath);
  const std::wstring data_directory = observed.drive_type == DRIVE_CDROM
      ? L"X:\\" + std::wstring(kWinPEDataRelativePath)
      : media_root + std::wstring(kWinPEDataRelativePath);
  const std::wstring payload_directory =
      std::filesystem::path(executable).parent_path().wstring();

  ytec::clonecore::UniqueHandle payload_handle;
  ytec::clonecore::UniqueHandle data_handle;
  ytec::clonecore::UniqueHandle executable_handle;
  if (!open_and_verify_normal_path(
          payload_directory, true, payload_handle) ||
      !open_and_verify_normal_path(data_directory, true, data_handle) ||
      !open_and_verify_normal_path(executable, false, executable_handle)) {
    std::wcerr << L"GUIまたはEXE隣dataを通常の非reparse実体として確認できません。\n";
    return 7;
  }

  std::wstring command_line = L"\"" + executable + L"\"";
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(
          executable.c_str(),
          command_line.data(),
          nullptr,
          nullptr,
          FALSE,
          0U,
          nullptr,
          payload_directory.c_str(),
          &startup,
          &process) == FALSE) {
    std::wcerr << L"検証済みレスキュー媒体からGUIを起動できませんでした。 Win32="
               << GetLastError() << L'\n';
    return 8;
  }
  ytec::clonecore::UniqueHandle process_handle(process.hProcess);
  ytec::clonecore::UniqueHandle thread_handle(process.hThread);
  static_cast<void>(thread_handle);
  if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) {
    std::wcerr << L"GUIの終了を確認できませんでした。 Win32="
               << GetLastError() << L'\n';
    return 9;
  }
  DWORD exit_code{};
  if (GetExitCodeProcess(process_handle.get(), &exit_code) == FALSE ||
      exit_code > static_cast<DWORD>((std::numeric_limits<int>::max)())) {
    std::wcerr << L"GUIの終了コードを安全に取得できませんでした。\n";
    return 10;
  }
  return static_cast<int>(exit_code);
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);

  if (argc == 2 && argv[1] != nullptr &&
      std::wstring_view(argv[1]) == kLaunchGuiFromMediaArgument) {
    return launch_gui_from_verified_rescue_media();
  }

  std::vector<std::wstring> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
  }
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const ytec::clonecore::Logger logger = ytec::clonecore::make_stderr_logger();
  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider(&logger);
  return ytec::winpeapp::run_winpe_app(
      arguments,
      *provider,
      std::cout,
      std::cerr);
}

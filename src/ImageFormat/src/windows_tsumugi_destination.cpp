#include "ytec/imageformat/windows_tsumugi_destination.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::size_t kExtentBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumExtentCount = 256U;

clonecore::Error destination_error(
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

clonecore::Status destination_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(destination_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(
          left.data(),
          static_cast<int>(left.size()),
          right.data(),
          static_cast<int>(right.size()),
          TRUE) == CSTR_EQUAL;
}

bool is_drive_absolute_path(const std::wstring_view path) {
  return path.size() >= 3U &&
      std::iswalpha(static_cast<wint_t>(path[0])) != 0 &&
      path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

bool has_tsumugi_extension(const std::wstring_view path) {
  constexpr std::wstring_view extension = L".tsumugi";
  return path.size() > extension.size() &&
      equals_ordinal_ignore_case(
          path.substr(path.size() - extension.size()), extension);
}

bool has_forbidden_path_character(const std::wstring_view path) {
  for (std::size_t index = 2U; index < path.size(); ++index) {
    const wchar_t character = path[index];
    if (character < 32 || character == L':' || character == L'*' ||
        character == L'?' || character == L'"' || character == L'<' ||
        character == L'>' || character == L'|') {
      return true;
    }
  }
  return false;
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

clonecore::Result<std::wstring> canonicalize_tsumugi_path(
    const std::wstring& requested) {
  if (!is_drive_absolute_path(requested) ||
      requested.size() >= kMaximumPathCharacters ||
      has_forbidden_path_character(requested) ||
      requested.ends_with(L"\\") || requested.ends_with(L"/") ||
      requested.ends_with(L" ") || requested.ends_with(L".")) {
    return clonecore::Result<std::wstring>::failure(destination_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"Tsumugi保存先パス検証",
        L"保存先はローカルドライブ上の絶対.tsumugiパスで指定してください"));
  }
  std::vector<wchar_t> buffer(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFullPathNameW(
      requested.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_argument,
            L"Tsumugi保存先の絶対パス化",
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::wstring canonical(buffer.data(), length);
  std::replace(canonical.begin(), canonical.end(), L'/', L'\\');
  if (!is_drive_absolute_path(canonical) ||
      !has_tsumugi_extension(canonical) ||
      has_forbidden_path_character(canonical)) {
    return clonecore::Result<std::wstring>::failure(destination_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"Tsumugi保存先パス検証",
        L"正規化後の保存先がローカル.tsumugiパスではありません"));
  }
  return clonecore::Result<std::wstring>::success(std::move(canonical));
}

clonecore::Result<std::wstring> parent_path(
    const std::wstring& canonical) {
  const std::size_t separator = canonical.find_last_of(L'\\');
  if (separator == std::wstring::npos || separator < 2U) {
    return clonecore::Result<std::wstring>::failure(destination_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"Tsumugi保存先親ディレクトリ",
        L"保存先の親ディレクトリを特定できません"));
  }
  if (separator == 2U) {
    return clonecore::Result<std::wstring>::success(
        canonical.substr(0U, 3U));
  }
  return clonecore::Result<std::wstring>::success(
      canonical.substr(0U, separator));
}

clonecore::Status reject_reparse_ancestors(
    const std::wstring& canonical) {
  const auto parent = parent_path(canonical);
  if (!parent) {
    return clonecore::Status::failure(parent.error());
  }
  std::size_t end = 3U;
  for (;;) {
    while (end < parent.value().size() &&
           parent.value()[end] != L'\\') {
      ++end;
    }
    std::wstring component = parent.value().substr(0U, end);
    if (component.size() == 2U) {
      component.push_back(L'\\');
    }
    const DWORD attributes =
        GetFileAttributesW(extended_path(component).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"Tsumugi保存先ancestor属性取得",
          GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return destination_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          L"Tsumugi保存先ancestor検証",
          L"全ancestorが通常ディレクトリである保存先だけを使用できます");
    }
    if (end >= parent.value().size()) {
      break;
    }
    ++end;
  }
  return clonecore::success_status();
}

clonecore::Result<WindowsImagePathIdentity> identity_from_handle(
    const HANDLE handle,
    const bool require_directory,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_ID_INFO id{};
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !GetFileInformationByHandleEx(
          handle, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard))) {
    return clonecore::Result<WindowsImagePathIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            GetLastError()));
  }
  const bool directory =
      (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      directory != require_directory || standard.EndOfFile.QuadPart < 0) {
    return clonecore::Result<WindowsImagePathIdentity>::failure(
        destination_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_REPARSE_TAG_INVALID,
            std::wstring(operation),
            L"reparse pointまたは予期しないオブジェクト種類を検出しました"));
  }
  WindowsImagePathIdentity result{
      .volume_serial_number = id.VolumeSerialNumber,
      .file_size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
  };
  static_assert(sizeof(id.FileId.Identifier) == 16U);
  std::memcpy(
      result.file_id.data(), id.FileId.Identifier, result.file_id.size());
  return clonecore::Result<WindowsImagePathIdentity>::success(result);
}

clonecore::Status verify_opened_path(
    const HANDLE handle,
    const std::wstring& expected_canonical,
    const std::wstring_view operation) {
  std::vector<wchar_t> actual(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle,
      actual.data(),
      static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0U || length >= actual.size()) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::identity_mismatch,
        std::wstring(operation),
        length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  if (!equals_ordinal_ignore_case(
          std::wstring_view(actual.data(), length),
          extended_path(expected_canonical))) {
    return destination_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ハンドルの実体パスが正規化済み予定パスと一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<WindowsImagePathIdentity> observe_parent_identity(
    const std::wstring& canonical) {
  const auto parent = parent_path(canonical);
  if (!parent) {
    return clonecore::Result<WindowsImagePathIdentity>::failure(
        parent.error());
  }
  clonecore::UniqueHandle handle(CreateFileW(
      extended_path(parent.value()).c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    return clonecore::Result<WindowsImagePathIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先親ディレクトリopen",
            GetLastError()));
  }
  auto path_status = verify_opened_path(
      handle.get(), parent.value(), L"Tsumugi保存先親実体パス再照合");
  if (!path_status) {
    return clonecore::Result<WindowsImagePathIdentity>::failure(
        path_status.error());
  }
  return identity_from_handle(
      handle.get(), true, L"Tsumugi保存先親ディレクトリ識別");
}

clonecore::Result<std::optional<WindowsImagePathIdentity>> observe_file(
    const std::wstring& canonical,
    const std::wstring_view operation) {
  clonecore::UniqueHandle handle(CreateFileW(
      extended_path(canonical).c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<
          std::optional<WindowsImagePathIdentity>>::success(std::nullopt);
    }
    return clonecore::Result<
        std::optional<WindowsImagePathIdentity>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            std::wstring(operation),
            native_code));
  }
  auto path_status = verify_opened_path(handle.get(), canonical, operation);
  if (!path_status) {
    return clonecore::Result<
        std::optional<WindowsImagePathIdentity>>::failure(
        path_status.error());
  }
  auto identity = identity_from_handle(handle.get(), false, operation);
  if (!identity) {
    return clonecore::Result<
        std::optional<WindowsImagePathIdentity>>::failure(identity.error());
  }
  return clonecore::Result<
      std::optional<WindowsImagePathIdentity>>::success(
      std::optional<WindowsImagePathIdentity>{identity.take_value()});
}

clonecore::Result<std::wstring> volume_root_for_path(
    const std::wstring& canonical) {
  std::vector<wchar_t> root(kMaximumPathCharacters, L'\0');
  if (!GetVolumePathNameW(
          extended_path(canonical).c_str(),
          root.data(),
          static_cast<DWORD>(root.size()))) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先Volume root取得",
            GetLastError()));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(root.data()));
}

clonecore::Result<std::uint32_t> destination_disk_number(
    const std::wstring& volume_root) {
  std::vector<wchar_t> volume_name(kMaximumPathCharacters, L'\0');
  if (!GetVolumeNameForVolumeMountPointW(
          volume_root.c_str(),
          volume_name.data(),
          static_cast<DWORD>(volume_name.size()))) {
    return clonecore::Result<std::uint32_t>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先Volume GUID取得",
            GetLastError()));
  }
  std::wstring open_path(volume_name.data());
  while (!open_path.empty() && open_path.back() == L'\\') {
    open_path.pop_back();
  }
  clonecore::UniqueHandle volume(CreateFileW(
      open_path.c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return clonecore::Result<std::uint32_t>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先Volume照会open",
            GetLastError()));
  }
  std::vector<std::byte> buffer(kExtentBufferBytes);
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
    return clonecore::Result<std::uint32_t>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先disk extent取得",
            GetLastError()));
  }
  constexpr std::size_t header_size =
      offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (returned < header_size) {
    return clonecore::Result<std::uint32_t>::failure(destination_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi保存先disk extent検証",
        L"disk extent応答が短すぎます"));
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents == 0U ||
      extents->NumberOfDiskExtents > kMaximumExtentCount ||
      header_size +
              static_cast<std::size_t>(extents->NumberOfDiskExtents) *
                  sizeof(DISK_EXTENT) >
          returned) {
    return clonecore::Result<std::uint32_t>::failure(destination_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi保存先disk extent検証",
        L"disk extent件数または応答境界が不正です"));
  }
  if (extents->NumberOfDiskExtents != 1U) {
    return clonecore::Result<std::uint32_t>::failure(destination_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi保存先disk extent検証",
        L"複数物理ディスクへまたがる保存先には対応しません"));
  }
  return clonecore::Result<std::uint32_t>::success(
      extents->Extents[0].DiskNumber);
}

struct DestinationInventory final {
  clonecore::StableDiskIdentity destination;
  std::vector<clonecore::StableDiskIdentity> connected;
};

clonecore::Result<DestinationInventory> observe_inventory(
    const std::uint32_t destination_number) {
  auto provider = diskmodel::make_windows_disk_inventory_provider();
  if (!provider) {
    return clonecore::Result<DestinationInventory>::failure(
        destination_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"Tsumugi保存先disk列挙準備",
            L"ディスク列挙器を作成できません"));
  }
  auto report = provider->enumerate();
  if (!report) {
    return clonecore::Result<DestinationInventory>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<DestinationInventory>::failure(
        destination_error(
            clonecore::ErrorCode::enumeration_failed,
            ERROR_INVALID_DATA,
            L"Tsumugi保存先disk再列挙",
            L"未解決のディスク列挙診断があるため保存を開始しません"));
  }
  std::vector<clonecore::StableDiskIdentity> connected;
  connected.reserve(report.value().disks.size());
  for (const auto& disk : report.value().disks) {
    auto identity =
        diskmodel::make_stable_disk_identity(disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<DestinationInventory>::failure(
          identity.error());
    }
    connected.push_back(identity.take_value());
  }
  const auto destination = std::find_if(
      connected.begin(),
      connected.end(),
      [&](const clonecore::StableDiskIdentity& candidate) {
        return candidate.disk_number == destination_number;
      });
  if (destination == connected.end()) {
    return clonecore::Result<DestinationInventory>::failure(
        destination_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Tsumugi保存先disk再識別",
            L"保存先Volumeの物理ディスクを再列挙できません"));
  }
  return clonecore::Result<DestinationInventory>::success(
      DestinationInventory{
          .destination = *destination,
          .connected = std::move(connected),
      });
}

WindowsImageDestinationFileSystem classify_file_system(
    const wchar_t* name) {
  if (_wcsicmp(name, L"NTFS") == 0) {
    return WindowsImageDestinationFileSystem::ntfs;
  }
  if (_wcsicmp(name, L"exFAT") == 0) {
    return WindowsImageDestinationFileSystem::exfat;
  }
  return WindowsImageDestinationFileSystem::other;
}

clonecore::Result<WindowsImageDestinationObservation> observe_destination(
    const std::wstring& requested) {
  auto canonical = canonicalize_tsumugi_path(requested);
  if (!canonical) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(canonical.error());
  }
  auto status = reject_reparse_ancestors(canonical.value());
  if (!status) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(status.error());
  }
  auto parent = observe_parent_identity(canonical.value());
  if (!parent) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(parent.error());
  }
  auto final = observe_file(
      canonical.value(), L"Tsumugi既存完成ファイル識別");
  if (!final) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(final.error());
  }
  const std::wstring partial_path = canonical.value() + L".partial";
  auto partial = observe_file(
      partial_path, L"Tsumugi未完了ファイル識別");
  if (!partial) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(partial.error());
  }
  auto root = volume_root_for_path(canonical.value());
  if (!root) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(root.error());
  }
  std::array<wchar_t, MAX_PATH + 1U> file_system_name{};
  if (!GetVolumeInformationW(
          root.value().c_str(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system_name.data(),
          static_cast<DWORD>(file_system_name.size()))) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先filesystem取得",
            GetLastError()));
  }
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(
          root.value().c_str(), &available, nullptr, nullptr)) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先空き容量取得",
            GetLastError()));
  }
  auto disk_number = destination_disk_number(root.value());
  if (!disk_number) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(disk_number.error());
  }
  auto inventory = observe_inventory(disk_number.value());
  if (!inventory) {
    return clonecore::Result<
        WindowsImageDestinationObservation>::failure(inventory.error());
  }
  return clonecore::Result<WindowsImageDestinationObservation>::success(
      WindowsImageDestinationObservation{
          .canonical_final_path = canonical.value(),
          .partial_path = partial_path,
          .destination_disk = inventory.value().destination,
          .connected_disks = std::move(inventory.value().connected),
          .available_bytes = available.QuadPart,
          .physical_disk_extent_count = 1U,
          .file_system = classify_file_system(file_system_name.data()),
          .parent_is_reparse = false,
          .final_exists = final.value().has_value(),
          .partial_exists = partial.value().has_value(),
          .parent_identity = parent.take_value(),
          .final_identity = final.take_value(),
          .partial_identity = partial.take_value(),
      });
}

bool same_stable_device(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) {
  if (!left.serial_suffix.empty() && !right.serial_suffix.empty() &&
      left.model == right.model &&
      left.serial_suffix == right.serial_suffix) {
    return true;
  }
  return !left.device_instance_id.empty() &&
      !right.device_instance_id.empty() &&
      left.device_instance_id == right.device_instance_id;
}

clonecore::Status validate_identity_self(
    const clonecore::StableDiskIdentity& identity,
    const std::wstring_view role) {
  return clonecore::validate_stable_identity(identity, identity, role);
}

clonecore::Result<clonecore::StableDiskIdentity> find_reidentified_disk(
    const clonecore::StableDiskIdentity& expected,
    const std::span<const clonecore::StableDiskIdentity> connected,
    const std::wstring_view role) {
  auto status = validate_identity_self(expected, role);
  if (!status) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        status.error());
  }
  std::vector<clonecore::StableDiskIdentity> matches;
  for (const auto& candidate : connected) {
    if (clonecore::validate_stable_identity(expected, candidate, role)) {
      matches.push_back(candidate);
    }
  }
  if (matches.size() != 1U) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        destination_error(
            clonecore::ErrorCode::identity_mismatch,
            matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
            std::wstring(role) + L"のTsumugi保存先再識別",
            matches.empty()
                ? L"選択ディスクを現在の列挙結果へ一意に対応付けできません"
                : L"同じ安定識別へ複数ディスクが対応しました"));
  }
  return clonecore::Result<clonecore::StableDiskIdentity>::success(
      matches.front());
}

clonecore::Status validate_guard_request(
    const WindowsTsumugiDestinationGuardRequest& request) {
  const bool before_stage =
      request.phase == WindowsTsumugiDestinationGuardPhase::before_stage;
  const bool before_commit =
      request.phase ==
      WindowsTsumugiDestinationGuardPhase::before_commit_owned_partial;
  if ((!before_stage && !before_commit) ||
      request.required_available_bytes == 0U ||
      (before_stage && request.expected_owned_partial_bytes != 0U) ||
      (before_commit && request.expected_owned_partial_bytes == 0U)) {
    return destination_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi保存先要求",
        L"検査段階、必要容量または所有.partial長が不正です");
  }
  auto canonical = canonicalize_tsumugi_path(request.final_path);
  if (!canonical) {
    return clonecore::Status::failure(canonical.error());
  }
  return validate_identity_self(request.expected_source_disk, L"コピー元");
}

}  // namespace

clonecore::Result<WindowsImageDestinationObservation>
observe_windows_tsumugi_destination(const std::wstring& final_path) {
  return observe_destination(final_path);
}

clonecore::Status validate_windows_tsumugi_destination_observation(
    const WindowsTsumugiDestinationGuardRequest& request,
    const WindowsImageDestinationObservation& value) {
  auto status = validate_guard_request(request);
  if (!status) {
    return status;
  }
  auto canonical = canonicalize_tsumugi_path(request.final_path);
  if (!canonical) {
    return clonecore::Status::failure(canonical.error());
  }
  if (!equals_ordinal_ignore_case(
          canonical.value(), value.canonical_final_path) ||
      !equals_ordinal_ignore_case(
          canonical.value() + L".partial", value.partial_path)) {
    return destination_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        L"Tsumugi保存先パス再識別",
        L"要求した完成パスと観測した完成／未完了パスが一致しません");
  }
  const bool before_stage =
      request.phase == WindowsTsumugiDestinationGuardPhase::before_stage;
  const bool invalid_partial =
      before_stage ? value.partial_exists : !value.partial_exists;
  const bool invalid_object_identity =
      !value.parent_identity.has_value() ||
      value.final_exists != value.final_identity.has_value() ||
      value.partial_exists != value.partial_identity.has_value();
  if (invalid_partial || (value.final_exists && !request.replace_existing)) {
    return destination_failure(
        clonecore::ErrorCode::confirmation_required,
        ERROR_FILE_EXISTS,
        L"Tsumugi保存先の既存状態",
        before_stage
            ? L"未知の.partialまたは未承認の完成ファイル置換を検出しました"
            : L"所有.partialの消失または未承認の完成ファイル置換を検出しました");
  }
  if (invalid_object_identity) {
    return destination_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi保存先object識別",
        L"親、完成ファイル、または.partialのhandle識別情報が観測状態と一致しません");
  }
  if (value.parent_is_reparse ||
      value.physical_disk_extent_count != 1U ||
      (value.file_system != WindowsImageDestinationFileSystem::ntfs &&
       value.file_system != WindowsImageDestinationFileSystem::exfat)) {
    return destination_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi保存先状態",
        L"reparse、複数disk、未対応filesystem、未知ファイル、または未承認置換を検出しました");
  }
  if (before_stage &&
      value.available_bytes < request.required_available_bytes) {
    return destination_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugi保存先空き容量",
        L"保存先空き容量が安全な作成上限より小さいため開始できません");
  }
  if (!before_stage &&
      value.partial_identity->file_size !=
          request.expected_owned_partial_bytes) {
    return destination_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi所有partial長再照合",
        L"検証済み.partialの観測長が作成結果と一致しません");
  }
  status = validate_identity_self(value.destination_disk, L"イメージ保存先");
  if (!status) {
    return status;
  }
  auto source = find_reidentified_disk(
      request.expected_source_disk, value.connected_disks, L"コピー元");
  if (!source) {
    return clonecore::Status::failure(source.error());
  }
  if (same_stable_device(source.value(), value.destination_disk)) {
    return destination_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"コピー元とTsumugi保存先の分離",
        L"コピー元ディスク上にはイメージを保存できません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_windows_tsumugi_destination(
    const WindowsTsumugiDestinationGuardRequest& request) {
  auto status = validate_guard_request(request);
  if (!status) {
    return status;
  }
  auto observation = observe_destination(request.final_path);
  if (!observation) {
    return clonecore::Status::failure(observation.error());
  }
  return validate_windows_tsumugi_destination_observation(
      request, observation.value());
}

}  // namespace ytec::imageformat

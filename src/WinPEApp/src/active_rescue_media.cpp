#include "ytec/winpeapp/active_rescue_media.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::size_t kRescueMediaMarkerBytes = 36U;
constexpr DWORD kMaximumLogicalDriveCharacters = 32U * 1024U;

clonecore::Error marker_error(
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
  return clonecore::Result<T>::failure(marker_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool valid_marker(const std::string_view marker) noexcept {
  if (marker.size() != kRescueMediaMarkerBytes) {
    return false;
  }
  bool has_nonzero_hex = false;
  for (std::size_t index = 0; index < marker.size(); ++index) {
    const unsigned char value =
        static_cast<unsigned char>(marker[index]);
    const bool hyphen_position =
        index == 8U || index == 13U || index == 18U || index == 23U;
    if (hyphen_position) {
      if (value != static_cast<unsigned char>('-')) {
        return false;
      }
      continue;
    }
    const bool hexadecimal =
        (value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9')) ||
        (value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('f')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('F'));
    if (!hexadecimal) {
      return false;
    }
    has_nonzero_hex = has_nonzero_hex || value != '0';
  }
  return has_nonzero_hex;
}

bool is_missing_marker_error(const DWORD native_code) noexcept {
  return native_code == ERROR_FILE_NOT_FOUND ||
         native_code == ERROR_PATH_NOT_FOUND;
}

clonecore::Result<std::optional<std::string>> read_marker_file(
    const std::wstring& marker_path) {
  if (marker_path.empty() || marker_path.size() >= 32U * 1024U) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"レスキュー媒体マーカーのパス検証",
        L"マーカーパスが空または長すぎます");
  }

  const std::filesystem::path path(marker_path);
  const std::filesystem::path parent = path.parent_path();
  if (!path.is_absolute() || parent.empty()) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"レスキュー媒体マーカーのパス検証",
        L"絶対パスの固定マーカーだけを読み取れます");
  }

  const DWORD parent_attributes = GetFileAttributesW(parent.c_str());
  if (parent_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD native_code = GetLastError();
    if (is_missing_marker_error(native_code)) {
      return clonecore::Result<std::optional<std::string>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"レスキュー媒体マーカー親フォルダーの確認",
            native_code));
  }
  if ((parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (parent_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"レスキュー媒体マーカー親フォルダーの確認",
        L"通常フォルダー以外またはreparse pointは使用できません");
  }

  const DWORD file_attributes = GetFileAttributesW(path.c_str());
  if (file_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD native_code = GetLastError();
    if (is_missing_marker_error(native_code)) {
      return clonecore::Result<std::optional<std::string>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"レスキュー媒体マーカー属性の確認",
            native_code));
  }
  if ((file_attributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE)) != 0U) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"レスキュー媒体マーカー属性の確認",
        L"通常ファイル以外またはreparse pointは使用できません");
  }

  clonecore::UniqueHandle marker(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!marker) {
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"レスキュー媒体マーカーを読取り専用で開く",
            GetLastError()));
  }

  DWORD handle_attributes = 0U;
  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (GetFileInformationByHandleEx(
          marker.get(), FileAttributeTagInfo, &tag_info, sizeof(tag_info))) {
    handle_attributes = tag_info.FileAttributes;
  } else {
    const DWORD tag_error = GetLastError();
    if (tag_error != ERROR_INVALID_PARAMETER) {
      return clonecore::Result<std::optional<std::string>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"レスキュー媒体マーカーハンドル属性の確認",
              tag_error));
    }

    // The WIM file-system driver used by WinPE can reject
    // FileAttributeTagInfo. Keep the same fail-closed attribute checks by
    // falling back only for that unsupported information class.
    BY_HANDLE_FILE_INFORMATION basic_info{};
    if (!GetFileInformationByHandle(marker.get(), &basic_info)) {
      return clonecore::Result<std::optional<std::string>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"レスキュー媒体マーカー基本属性の確認",
              GetLastError()));
    }
    handle_attributes = basic_info.dwFileAttributes;
  }
  if ((handle_attributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE)) != 0U ||
      GetFileType(marker.get()) != FILE_TYPE_DISK) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_INVALID_DATA,
        L"レスキュー媒体マーカーハンドル属性の確認",
        L"通常の非reparseファイルだけを読み取れます");
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(marker.get(), &size)) {
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"レスキュー媒体マーカーサイズの確認",
            GetLastError()));
  }
  if (size.QuadPart != static_cast<LONGLONG>(kRescueMediaMarkerBytes)) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"レスキュー媒体マーカーサイズの確認",
        L"マーカーは36バイトのGUID形式である必要があります");
  }

  std::array<char, kRescueMediaMarkerBytes> bytes{};
  DWORD read_bytes = 0;
  if (!ReadFile(
          marker.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &read_bytes,
          nullptr)) {
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"レスキュー媒体マーカーの読取り",
            GetLastError()));
  }
  if (read_bytes != bytes.size()) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::io_failed,
        ERROR_HANDLE_EOF,
        L"レスキュー媒体マーカーの読取り",
        L"マーカー全体を読み取れませんでした");
  }

  std::string value(bytes.begin(), bytes.end());
  if (!valid_marker(value)) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"レスキュー媒体マーカー内容の確認",
        L"マーカーが有界ASCII GUID形式ではありません");
  }
  return clonecore::Result<std::optional<std::string>>::success(
      std::move(value));
}

bool valid_drive_root(const std::wstring_view root) noexcept {
  return root.size() == 3U &&
         ((root[0] >= L'A' && root[0] <= L'Z') ||
          (root[0] >= L'a' && root[0] <= L'z')) &&
         root[1] == L':' && (root[2] == L'\\' || root[2] == L'/');
}

bool same_drive_root(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return valid_drive_root(left) && valid_drive_root(right) &&
         CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

clonecore::Result<std::vector<std::wstring>> enumerate_local_drive_roots() {
  const DWORD required = GetLogicalDriveStringsW(0, nullptr);
  if (required == 0U) {
    return clonecore::Result<std::vector<std::wstring>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::enumeration_failed,
            L"ローカルドライブ列挙サイズの取得",
            GetLastError()));
  }
  if (required > kMaximumLogicalDriveCharacters) {
    return failure<std::vector<std::wstring>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"ローカルドライブ列挙サイズの確認",
        L"ドライブ文字列が安全上限を超えています");
  }

  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U, L'\0');
  const DWORD copied = GetLogicalDriveStringsW(
      static_cast<DWORD>(buffer.size()), buffer.data());
  if (copied == 0U ||
      static_cast<std::size_t>(copied) >= buffer.size()) {
    return clonecore::Result<std::vector<std::wstring>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::enumeration_failed,
            L"ローカルドライブの列挙",
            copied == 0U ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
  }

  std::vector<std::wstring> roots;
  std::size_t offset = 0U;
  while (offset < copied) {
    const wchar_t* begin = buffer.data() + offset;
    const std::size_t remaining = static_cast<std::size_t>(copied) - offset;
    const std::size_t length = wcsnlen_s(begin, remaining);
    if (length == 0U || length == remaining) {
      return failure<std::vector<std::wstring>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ローカルドライブ列挙結果の確認",
          L"ドライブ文字列の終端または内容が不正です");
    }
    std::wstring root(begin, length);
    if (!valid_drive_root(root)) {
      return failure<std::vector<std::wstring>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DRIVE,
          L"ローカルドライブルートの確認",
          L"ドライブ文字形式ではないルートを検出しました");
    }

    const UINT drive_type = GetDriveTypeW(root.c_str());
    if (drive_type == DRIVE_UNKNOWN || drive_type == DRIVE_NO_ROOT_DIR) {
      return failure<std::vector<std::wstring>>(
          clonecore::ErrorCode::query_failed,
          ERROR_NOT_READY,
          L"ローカルドライブ種別の確認",
          L"列挙されたドライブの種別を確定できません");
    }
    if (drive_type != DRIVE_REMOTE) {
      const bool duplicate = std::any_of(
          roots.begin(), roots.end(), [&](const auto& existing) {
            return same_drive_root(existing, root);
          });
      if (duplicate) {
        return failure<std::vector<std::wstring>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"ローカルドライブルートの一意性確認",
            L"同じドライブルートが複数回列挙されました");
      }
      roots.push_back(std::move(root));
    }
    offset += length + 1U;
  }
  return clonecore::Result<std::vector<std::wstring>>::success(
      std::move(roots));
}

clonecore::Result<std::uint32_t> query_local_drive_type(
    const std::wstring& root) {
  if (!valid_drive_root(root)) {
    return failure<std::uint32_t>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DRIVE,
        L"PE起動媒体ドライブ種別の確認",
        L"ドライブ文字形式ではないルートは照会できません");
  }
  const UINT drive_type = GetDriveTypeW(root.c_str());
  if (drive_type == DRIVE_UNKNOWN || drive_type == DRIVE_NO_ROOT_DIR) {
    return failure<std::uint32_t>(
        clonecore::ErrorCode::query_failed,
        ERROR_NOT_READY,
        L"PE起動媒体ドライブ種別の確認",
        L"媒体マーカーのあるドライブ種別を確定できません");
  }
  return clonecore::Result<std::uint32_t>::success(drive_type);
}

clonecore::Result<diskmodel::InventoryReport> enumerate_disks() {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (!inventory) {
    return failure<diskmodel::InventoryReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"PE起動媒体ディスク一覧の準備",
        L"ディスク一覧プロバイダーを作成できませんでした");
  }
  return inventory->enumerate();
}

}  // namespace

clonecore::Result<ActiveRescueMediaStorageObservation>
resolve_active_rescue_media_storage(
    const ActiveRescueMediaDependencies& dependencies) {
  if (!dependencies.read_marker ||
      !dependencies.enumerate_local_drive_roots ||
      !dependencies.query_local_drive_type ||
      !dependencies.query_single_disk_number_for_path ||
      !dependencies.enumerate_disks) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE起動媒体照合依存の確認",
        L"読取り専用の媒体照合依存が不足しています");
  }

  const auto runtime_marker = dependencies.read_marker(
      std::wstring(kActiveRescueMediaRuntimeMarkerPath));
  if (!runtime_marker) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
        runtime_marker.error());
  }
  if (!runtime_marker.value().has_value()) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_NOT_FOUND,
        L"WinPE内レスキュー媒体マーカーの確認",
        L"起動中WinPEに媒体マーカーがありません。既存媒体は再作成してください");
  }
  if (!valid_marker(*runtime_marker.value())) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinPE内レスキュー媒体マーカーの確認",
        L"起動中WinPEの媒体マーカー形式が不正です");
  }

  const auto roots = dependencies.enumerate_local_drive_roots();
  if (!roots) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
        roots.error());
  }

  std::vector<std::wstring> matching_paths;
  for (const auto& root : roots.value()) {
    if (!valid_drive_root(root)) {
      return failure<ActiveRescueMediaStorageObservation>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DRIVE,
          L"PE起動媒体候補ルートの確認",
          L"ドライブ文字形式ではない候補を検出しました");
    }
    if (same_drive_root(root, L"X:\\")) {
      continue;
    }
    const std::wstring marker_path =
        root + std::wstring(kActiveRescueMediaMarkerRelativePath);
    const auto marker = dependencies.read_marker(marker_path);
    if (!marker) {
      return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
          marker.error());
    }
    if (!marker.value().has_value()) {
      continue;
    }
    if (!valid_marker(*marker.value())) {
      return failure<ActiveRescueMediaStorageObservation>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE起動媒体候補マーカーの確認",
          L"ローカルドライブ上の媒体マーカー形式が不正です");
    }
    if (*marker.value() == *runtime_marker.value()) {
      matching_paths.push_back(marker_path);
    }
  }

  if (matching_paths.size() != 1U) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::identity_mismatch,
        matching_paths.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"PE起動媒体候補の一意性確認",
        matching_paths.empty()
            ? L"起動中WinPEと同じ媒体マーカーを持つローカルドライブがありません"
             : L"起動中WinPEと同じ媒体マーカーを持つローカルドライブが複数あります");
  }

  const std::filesystem::path matching_path(matching_paths.front());
  const std::wstring matching_root = matching_path.root_path().wstring();
  if (!valid_drive_root(matching_root)) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DRIVE,
        L"PE起動媒体候補ルートの確認",
        L"一意に選んだ媒体マーカーのルートが不正です");
  }
  const auto drive_type = dependencies.query_local_drive_type(matching_root);
  if (!drive_type) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
        drive_type.error());
  }
  if (drive_type.value() != DRIVE_FIXED &&
      drive_type.value() != DRIVE_REMOVABLE &&
      drive_type.value() != DRIVE_CDROM) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE起動媒体ドライブ種別の確認",
        L"固定、リムーバブル、光学媒体以外の起動媒体は対応しません");
  }

  // read_marker is an opened-handle contract. Reopening here, followed by a
  // second drive classification, binds the selected path to the identity and
  // media class that are about to be consumed.
  const auto locked_marker = dependencies.read_marker(matching_paths.front());
  if (!locked_marker || !locked_marker.value().has_value() ||
      *locked_marker.value() != *runtime_marker.value()) {
    if (!locked_marker) {
      return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
          locked_marker.error());
    }
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE起動媒体マーカーのopened-handle再確認",
        L"ドライブ種別の確認直後に媒体マーカーが変化または消失しました");
  }
  const auto locked_drive_type =
      dependencies.query_local_drive_type(matching_root);
  if (!locked_drive_type || locked_drive_type.value() != drive_type.value()) {
    if (!locked_drive_type) {
      return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
          locked_drive_type.error());
    }
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE起動媒体ドライブ種別の再確認",
        L"媒体マーカーのあるドライブ種別が照会中に変化しました");
  }

  ActiveRescueMediaStorageObservation observation{
      .marker_path = matching_paths.front(),
      .drive_type = locked_drive_type.value(),
      .physical_identity = std::nullopt,
      .marker_identity_from_open_handle = true,
  };
  if (observation.drive_type == DRIVE_CDROM) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::success(
        std::move(observation));
  }

  const auto disk_number = dependencies.query_single_disk_number_for_path(
      matching_paths.front());
  if (!disk_number) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
        disk_number.error());
  }
  const auto mapped_marker = dependencies.read_marker(
      matching_paths.front());
  if (!mapped_marker || !mapped_marker.value().has_value() ||
      *mapped_marker.value() != *runtime_marker.value()) {
    if (!mapped_marker) {
      return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
          mapped_marker.error());
    }
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE起動媒体マーカーの再確認",
        L"物理ディスク対応の直後に媒体マーカーが変化または消失しました");
  }

  const auto inventory = dependencies.enumerate_disks();
  if (!inventory) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
        inventory.error());
  }
  const auto matches_number = [&](const diskmodel::DiskInfo& disk) {
    return disk.disk_number == disk_number.value();
  };
  const auto count = static_cast<std::size_t>(std::count_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      matches_number));
  if (count != 1U) {
    return failure<ActiveRescueMediaStorageObservation>(
        clonecore::ErrorCode::identity_mismatch,
        count == 0U ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"PE起動媒体物理ディスクの一意性確認",
        count == 0U
            ? L"媒体マーカーのあるボリュームを物理ディスク一覧へ対応付けできません"
            : L"同じディスク番号を持つ物理ディスクが複数あります");
  }
  const auto observed = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      matches_number);
  auto observed_identity = diskmodel::make_stable_disk_identity(
      *observed, observed->is_system_disk);
  if (!observed_identity) {
    return clonecore::Result<ActiveRescueMediaStorageObservation>::failure(
        observed_identity.error());
  }

  observation.physical_identity = observed_identity.take_value();
  return clonecore::Result<ActiveRescueMediaStorageObservation>::success(
      std::move(observation));
}

clonecore::Result<bool> resolve_active_rescue_media_target(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    const ActiveRescueMediaDependencies& dependencies) {
  // This read-only identity resolver deliberately does not reinterpret OK.
  static_cast<void>(confirmation);

  const auto expected_identity_status = clonecore::validate_stable_identity(
      expected_target, expected_target, L"復元先");
  if (!expected_identity_status) {
    return clonecore::Result<bool>::failure(
        expected_identity_status.error());
  }

  auto storage = resolve_active_rescue_media_storage(dependencies);
  if (!storage) {
    return clonecore::Result<bool>::failure(storage.error());
  }
  if (!storage.value().physical_identity.has_value()) {
    // A verified optical boot medium cannot be a physical-disk target.
    return clonecore::Result<bool>::success(false);
  }

  const auto identity_status = clonecore::validate_stable_identity(
      expected_target,
      *storage.value().physical_identity,
      L"PE起動媒体");
  if (!identity_status) {
    return clonecore::Result<bool>::success(false);
  }
  return clonecore::Result<bool>::success(true);
}

ActiveRescueMediaDependencies
make_active_rescue_media_windows_dependencies() {
  return ActiveRescueMediaDependencies{
      .read_marker = read_marker_file,
      .enumerate_local_drive_roots = enumerate_local_drive_roots,
      .query_local_drive_type = query_local_drive_type,
      .query_single_disk_number_for_path =
          diskmodel::query_single_disk_number_for_local_path,
      .enumerate_disks = enumerate_disks,
  };
}

clonecore::Result<ActiveRescueMediaStorageObservation>
query_active_rescue_media_storage_with_windows_apis() {
  return resolve_active_rescue_media_storage(
      make_active_rescue_media_windows_dependencies());
}

clonecore::Result<bool> query_active_rescue_media_target_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation) {
  return resolve_active_rescue_media_target(
      expected_target,
      confirmation,
      make_active_rescue_media_windows_dependencies());
}

}  // namespace ytec::winpeapp

#include "ytec/imageformat/windows_file_staging.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <Windows.h>
#include <sddl.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::size_t kExtentBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumExtentCount = 256;

clonecore::Error staging_error(
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

clonecore::Status staging_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(staging_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_drive_absolute_path(const std::wstring_view path) {
  return path.size() >= 3 &&
         std::iswalpha(static_cast<wint_t>(path[0])) != 0 &&
         path[1] == L':' &&
         (path[2] == L'\\' || path[2] == L'/');
}

bool has_dcimg_extension(const std::wstring_view path) {
  constexpr std::wstring_view extension = L".dcimg";
  return path.size() > extension.size() &&
         equals_ordinal_ignore_case(
             path.substr(path.size() - extension.size()), extension);
}

bool has_forbidden_path_character(const std::wstring_view path) {
  for (std::size_t index = 2; index < path.size(); ++index) {
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

clonecore::Result<std::wstring> canonicalize_final_path(
    const std::wstring& requested) {
  if (!is_drive_absolute_path(requested) ||
      requested.size() >= kMaximumPathCharacters ||
      has_forbidden_path_character(requested) ||
      requested.ends_with(L"\\") || requested.ends_with(L"/") ||
      requested.ends_with(L" ") || requested.ends_with(L".")) {
    return clonecore::Result<std::wstring>::failure(staging_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"イメージ保存先パス検証",
        L"保存先はローカルドライブ上の絶対イメージパスで指定してください"));
  }

  std::vector<wchar_t> buffer(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFullPathNameW(
      requested.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (length == 0 || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_argument,
            L"イメージ保存先の絶対パス化",
            length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::wstring canonical(buffer.data(), length);
  std::replace(canonical.begin(), canonical.end(), L'/', L'\\');
  if (!is_drive_absolute_path(canonical) ||
      !has_dcimg_extension(canonical) ||
      has_forbidden_path_character(canonical)) {
    return clonecore::Result<std::wstring>::failure(staging_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"イメージ保存先パス検証",
        L"保存先はローカルドライブ上の絶対.dcimgパスでなければなりません"));
  }
  return clonecore::Result<std::wstring>::success(std::move(canonical));
}

clonecore::Status reject_reparse_path(
    const std::wstring& canonical_final_path) {
  const std::size_t separator = canonical_final_path.find_last_of(L'\\');
  if (separator == std::wstring::npos || separator < 2) {
    return staging_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"イメージ保存先親ディレクトリ検証",
        L"保存先の親ディレクトリを特定できません");
  }
  const std::wstring parent = canonical_final_path.substr(0, separator);
  std::size_t end = 3;
  for (;;) {
    while (end < parent.size() && parent[end] != L'\\') {
      ++end;
    }
    std::wstring component = parent.substr(0, end);
    if (component.size() == 2) {
      component.push_back(L'\\');
    }
    const DWORD attributes =
        GetFileAttributesW(extended_path(component).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"イメージ保存先親ディレクトリ属性取得",
          GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return staging_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_DIRECTORY,
          L"イメージ保存先親ディレクトリ検証",
          L"保存先の親要素がディレクトリではありません");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return staging_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          L"イメージ保存先reparse検証",
          L"reparse pointを経由する保存先は使用できません");
    }
    if (end >= parent.size()) {
      break;
    }
    ++end;
  }
  return clonecore::success_status();
}

clonecore::Result<bool> path_exists(const std::wstring& canonical_path) {
  const DWORD attributes =
      GetFileAttributesW(extended_path(canonical_path).c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return clonecore::Result<bool>::success(true);
  }
  const DWORD native_code = GetLastError();
  if (native_code == ERROR_FILE_NOT_FOUND ||
      native_code == ERROR_PATH_NOT_FOUND) {
    return clonecore::Result<bool>::success(false);
  }
  return clonecore::Result<bool>::failure(clonecore::make_win32_error(
      clonecore::ErrorCode::query_failed,
      L"イメージ保存先ファイル属性取得",
      native_code));
}

clonecore::Result<std::wstring> volume_root_for_path(
    const std::wstring& canonical_path) {
  std::vector<wchar_t> root(kMaximumPathCharacters, L'\0');
  if (!GetVolumePathNameW(
          extended_path(canonical_path).c_str(),
          root.data(),
          static_cast<DWORD>(root.size()))) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"イメージ保存先ボリュームルート取得",
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
            L"イメージ保存先Volume GUID取得",
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
            L"イメージ保存先ボリューム照会オープン",
            GetLastError()));
  }

  std::vector<std::byte> buffer(kExtentBufferBytes);
  DWORD returned = 0;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &returned,
          nullptr)) {
    return clonecore::Result<std::uint32_t>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"イメージ保存先ディスク範囲取得",
            GetLastError()));
  }
  constexpr std::size_t header_size =
      offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (returned < header_size) {
    return clonecore::Result<std::uint32_t>::failure(staging_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"イメージ保存先ディスク範囲検証",
        L"ディスク範囲応答が短すぎます"));
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents == 0 ||
      extents->NumberOfDiskExtents > kMaximumExtentCount) {
    return clonecore::Result<std::uint32_t>::failure(staging_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"イメージ保存先ディスク範囲検証",
        L"ディスク範囲件数が不正です"));
  }
  const std::size_t required =
      header_size +
      static_cast<std::size_t>(extents->NumberOfDiskExtents) *
          sizeof(DISK_EXTENT);
  if (required > returned) {
    return clonecore::Result<std::uint32_t>::failure(staging_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"イメージ保存先ディスク範囲検証",
        L"全ディスク範囲が応答に含まれていません"));
  }
  if (extents->NumberOfDiskExtents != 1) {
    return clonecore::Result<std::uint32_t>::failure(staging_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"イメージ保存先ディスク範囲検証",
        L"複数ディスクへまたがる保存先には対応しません"));
  }
  return clonecore::Result<std::uint32_t>::success(
      extents->Extents[0].DiskNumber);
}

struct DestinationInventoryObservation final {
  clonecore::StableDiskIdentity destination;
  std::vector<clonecore::StableDiskIdentity> connected_disks;
};

clonecore::Result<DestinationInventoryObservation>
observe_destination_inventory(const std::uint32_t disk_number) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (!inventory) {
    return clonecore::Result<DestinationInventoryObservation>::failure(
        staging_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"イメージ保存先ディスク列挙準備",
            L"ディスク列挙器を作成できません"));
  }
  auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<DestinationInventoryObservation>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<DestinationInventoryObservation>::failure(
        staging_error(
            clonecore::ErrorCode::enumeration_failed,
            ERROR_INVALID_DATA,
            L"イメージ保存先ディスク再列挙",
            L"未解決のディスク列挙診断があるため保存を開始しません"));
  }
  std::vector<clonecore::StableDiskIdentity> connected;
  connected.reserve(report.value().disks.size());
  for (const auto& disk : report.value().disks) {
    const auto identity =
        diskmodel::make_stable_disk_identity(disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<DestinationInventoryObservation>::failure(
          identity.error());
    }
    connected.push_back(identity.value());
  }
  const auto destination = std::find_if(
      connected.begin(),
      connected.end(),
      [&](const clonecore::StableDiskIdentity& candidate) {
        return candidate.disk_number == disk_number;
      });
  if (destination == connected.end()) {
    return clonecore::Result<DestinationInventoryObservation>::failure(
        staging_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"イメージ保存先ディスク再識別",
            L"保存先ボリュームの物理ディスクを再列挙できません"));
  }
  return clonecore::Result<DestinationInventoryObservation>::success(
      DestinationInventoryObservation{
          .destination = *destination,
          .connected_disks = std::move(connected),
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

clonecore::Result<clonecore::StableDiskIdentity>
find_reidentified_disk(
    const clonecore::StableDiskIdentity& expected,
    const std::span<const clonecore::StableDiskIdentity> connected,
    const std::wstring_view role) {
  const auto expected_status = validate_identity_self(expected, role);
  if (!expected_status) {
    return clonecore::Result<
        clonecore::StableDiskIdentity>::failure(expected_status.error());
  }
  std::vector<clonecore::StableDiskIdentity> matches;
  for (const auto& candidate : connected) {
    if (clonecore::validate_stable_identity(
            expected, candidate, role)) {
      matches.push_back(candidate);
    }
  }
  if (matches.size() != 1) {
    return clonecore::Result<
        clonecore::StableDiskIdentity>::failure(staging_error(
            clonecore::ErrorCode::identity_mismatch,
            matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
            std::wstring(role) + L"の保存開始前再識別",
            matches.empty()
                ? L"選択したディスクを現在の再列挙結果へ一意に対応付けできません"
                : L"同じ安定識別へ複数ディスクが対応したため開始できません"));
  }
  return clonecore::Result<
      clonecore::StableDiskIdentity>::success(matches.front());
}

clonecore::Status validate_initial_observation(
    const WindowsFileStagingRequest& request,
    const WindowsFileDestinationObservation& observation,
    const std::uint64_t expected_length) {
  if (expected_length == 0 ||
      expected_length >
          static_cast<std::uint64_t>(
              std::numeric_limits<LONGLONG>::max())) {
    return staging_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"イメージ段階ファイル長検証",
        L"作成予定長が安全な範囲外です");
  }
  if (!is_drive_absolute_path(observation.canonical_final_path) ||
      !has_dcimg_extension(observation.canonical_final_path) ||
      observation.partial_path !=
          observation.canonical_final_path + L".partial") {
    return staging_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"イメージ保存先観測結果検証",
        L"正規化済み完成パスまたは未完了パスが不正です");
  }
  if (observation.parent_is_reparse) {
    return staging_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"イメージ保存先reparse検証",
        L"reparse pointを経由する保存先は使用できません");
  }
  if (observation.final_exists || observation.partial_exists) {
    return staging_failure(
        clonecore::ErrorCode::confirmation_required,
        ERROR_FILE_EXISTS,
        L"イメージ保存先の非上書き検証",
        L"完成ファイルまたは未完了ファイルが既に存在します");
  }
  if (observation.available_bytes < expected_length) {
    return staging_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"イメージ保存先空き容量検証",
        L"保存先の空き容量が作成予定長より小さいため開始できません");
  }

  auto status =
      validate_identity_self(observation.destination_disk, L"イメージ保存先");
  if (!status) {
    return status;
  }
  const auto observed_source = find_reidentified_disk(
      request.expected_source_disk,
      observation.connected_disks,
      L"コピー元");
  if (!observed_source) {
    return clonecore::Status::failure(observed_source.error());
  }
  if (same_stable_device(
          observed_source.value(), observation.destination_disk)) {
    return staging_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"コピー元とイメージ保存先の分離",
        L"コピー元ディスク上にはイメージを保存できません");
  }
  if (request.expected_clone_target_disk.has_value()) {
    const auto observed_target = find_reidentified_disk(
        *request.expected_clone_target_disk,
        observation.connected_disks,
        L"コピー先");
    if (!observed_target) {
      return clonecore::Status::failure(observed_target.error());
    }
    if (same_stable_device(
            observed_target.value(),
            observation.destination_disk)) {
      return staging_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DRIVE,
          L"コピー先とイメージ保存先の分離",
          L"コピー先ディスク上にはイメージを保存できません");
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_active_observation(
    const WindowsFileStagingRequest& request,
    const WindowsFileDestinationObservation& initial,
    const WindowsFileDestinationObservation& current) {
  if (!equals_ordinal_ignore_case(
          initial.canonical_final_path, current.canonical_final_path) ||
      !equals_ordinal_ignore_case(
          initial.partial_path, current.partial_path) ||
      current.parent_is_reparse || current.final_exists ||
      !current.partial_exists) {
    return staging_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"イメージ保存先パスの再確認",
        L"開始時と確定直前の保存先状態が一致しません");
  }
  auto status = clonecore::validate_stable_identity(
      initial.destination_disk,
      current.destination_disk,
      L"イメージ保存先");
  if (!status) {
    return status;
  }
  const auto source = find_reidentified_disk(
      request.expected_source_disk,
      current.connected_disks,
      L"コピー元");
  if (!source) {
    return clonecore::Status::failure(source.error());
  }
  if (same_stable_device(source.value(), current.destination_disk)) {
    return staging_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"コピー元とイメージ保存先の確定前分離",
        L"確定前にコピー元と保存先が同一ディスクを示しました");
  }
  if (request.expected_clone_target_disk.has_value()) {
    const auto target = find_reidentified_disk(
        *request.expected_clone_target_disk,
        current.connected_disks,
        L"コピー先");
    if (!target) {
      return clonecore::Status::failure(target.error());
    }
    if (same_stable_device(target.value(), current.destination_disk)) {
      return staging_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DRIVE,
          L"コピー先とイメージ保存先の確定前分離",
          L"確定前にコピー先と保存先が同一ディスクを示しました");
    }
  }
  return clonecore::success_status();
}

class LocalMemory final {
 public:
  LocalMemory() noexcept = default;
  explicit LocalMemory(HLOCAL value) noexcept : value_(value) {}
  ~LocalMemory() noexcept {
    if (value_ != nullptr) {
      LocalFree(value_);
    }
  }
  LocalMemory(const LocalMemory&) = delete;
  LocalMemory& operator=(const LocalMemory&) = delete;
  LocalMemory(LocalMemory&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  LocalMemory& operator=(LocalMemory&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) {
        LocalFree(value_);
      }
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] HLOCAL get() const noexcept { return value_; }

 private:
  HLOCAL value_{};
};

clonecore::Result<LocalMemory> restricted_security_descriptor() {
  clonecore::UniqueHandle token;
  HANDLE raw_token = INVALID_HANDLE_VALUE;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            L"イメージ一時ファイル用トークン取得",
            GetLastError()));
  }
  token.reset(raw_token);

  DWORD bytes = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
  const DWORD size_error = GetLastError();
  if (bytes == 0 || size_error != ERROR_INSUFFICIENT_BUFFER) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"イメージ一時ファイル用ユーザーSID寸法取得",
            size_error));
  }
  std::vector<std::byte> token_user(bytes);
  if (!GetTokenInformation(
          token.get(),
          TokenUser,
          token_user.data(),
          static_cast<DWORD>(token_user.size()),
          &bytes)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"イメージ一時ファイル用ユーザーSID取得",
            GetLastError()));
  }
  const auto* user =
      reinterpret_cast<const TOKEN_USER*>(token_user.data());
  LPWSTR sid_text_raw = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &sid_text_raw)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"イメージ一時ファイル用ユーザーSID文字列化",
            GetLastError()));
  }
  LocalMemory sid_text(sid_text_raw);
  const std::wstring sid(static_cast<const wchar_t*>(sid_text.get()));
  const std::wstring sddl =
      L"O:" + sid + L"D:P(A;;FA;;;" + sid +
      L")(A;;FA;;;SY)(A;;FA;;;BA)";
  PSECURITY_DESCRIPTOR descriptor_raw = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(),
          SDDL_REVISION_1,
          &descriptor_raw,
          nullptr)) {
    return clonecore::Result<LocalMemory>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::internal_error,
            L"イメージ一時ファイル用DACL作成",
            GetLastError()));
  }
  return clonecore::Result<LocalMemory>::success(
      LocalMemory(descriptor_raw));
}

class Win32FileStagingBackend final
    : public IWindowsFileStagingBackend {
 public:
  ~Win32FileStagingBackend() override {
    if (handle_) {
      handle_.reset();
    }
    if (owns_partial_ && !owned_partial_path_.empty()) {
      DeleteFileW(extended_path(owned_partial_path_).c_str());
    }
  }

  [[nodiscard]] clonecore::Result<WindowsFileDestinationObservation>
  observe_destination(
      const std::wstring& requested_final_path) override {
    const auto canonical = canonicalize_final_path(requested_final_path);
    if (!canonical) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(canonical.error());
    }
    const auto reparse = reject_reparse_path(canonical.value());
    if (!reparse) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(reparse.error());
    }

    const std::wstring partial = canonical.value() + L".partial";
    const auto final_exists = path_exists(canonical.value());
    if (!final_exists) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(
              final_exists.error());
    }
    const auto partial_exists = path_exists(partial);
    if (!partial_exists) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(
              partial_exists.error());
    }
    const auto root = volume_root_for_path(canonical.value());
    if (!root) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(root.error());
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(
            root.value().c_str(), &available, nullptr, nullptr)) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::query_failed,
                  L"イメージ保存先空き容量取得",
                  GetLastError()));
    }
    const auto disk_number = destination_disk_number(root.value());
    if (!disk_number) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(
              disk_number.error());
    }
    const auto inventory =
        observe_destination_inventory(disk_number.value());
    if (!inventory) {
      return clonecore::Result<
          WindowsFileDestinationObservation>::failure(inventory.error());
    }
    return clonecore::Result<
        WindowsFileDestinationObservation>::success(
            WindowsFileDestinationObservation{
                .canonical_final_path = canonical.value(),
                .partial_path = partial,
                .destination_disk = inventory.value().destination,
                .connected_disks =
                    inventory.value().connected_disks,
                .available_bytes = available.QuadPart,
                .parent_is_reparse = false,
                .final_exists = final_exists.value(),
                .partial_exists = partial_exists.value(),
            });
  }

  [[nodiscard]] clonecore::Status create_new_restricted_partial(
      const std::wstring& partial_path,
      const std::uint64_t expected_length) override {
    if (handle_ || owns_partial_ || expected_length == 0 ||
        expected_length >
            static_cast<std::uint64_t>(
                std::numeric_limits<LONGLONG>::max())) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"イメージ未完了ファイル作成",
          L"ファイル状態または作成予定長が不正です");
    }
    const auto descriptor = restricted_security_descriptor();
    if (!descriptor) {
      return clonecore::Status::failure(descriptor.error());
    }
    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.value().get(),
        .bInheritHandle = FALSE,
    };
    const std::wstring native_path = extended_path(partial_path);
    HANDLE raw_handle = CreateFileW(
        native_path.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
            FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (raw_handle == INVALID_HANDLE_VALUE) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイルの新規作成",
          GetLastError()));
    }
    handle_.reset(raw_handle);
    owns_partial_ = true;
    owned_partial_path_ = partial_path;
    expected_length_ = expected_length;

    std::vector<wchar_t> actual(kMaximumPathCharacters, L'\0');
    const DWORD actual_length = GetFinalPathNameByHandleW(
        handle_.get(),
        actual.data(),
        static_cast<DWORD>(actual.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (actual_length == 0 || actual_length >= actual.size()) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::identity_mismatch,
          L"イメージ未完了ファイル実体パス取得",
          actual_length == 0
              ? GetLastError()
              : ERROR_FILENAME_EXCED_RANGE));
    }
    if (!equals_ordinal_ignore_case(
            std::wstring_view(actual.data(), actual_length),
            native_path)) {
      return staging_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_NAME,
          L"イメージ未完了ファイル実体パス検証",
          L"作成したファイルの実体パスが予定パスと一致しません");
    }

    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(expected_length);
    if (!SetFilePointerEx(handle_.get(), end, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle_.get())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイル長の予約",
          GetLastError()));
    }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(
            handle_.get(), beginning, nullptr, FILE_BEGIN)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイル位置の初期化",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] bool owns_partial() const noexcept override {
    return owns_partial_;
  }

  [[nodiscard]] clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (!handle_ || offset > expected_length_ ||
        bytes.size() > expected_length_ - offset) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"イメージ未完了ファイル書込み範囲",
          L"書込み範囲が作成予定長の外です");
    }
    auto status = seek(offset, L"イメージ未完了ファイル書込み位置");
    if (!status) {
      return status;
    }
    std::size_t completed = 0;
    while (completed < bytes.size()) {
      const DWORD block = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - completed,
          std::numeric_limits<DWORD>::max()));
      DWORD written = 0;
      const BOOL succeeded = WriteFile(
          handle_.get(),
          bytes.data() + completed,
          block,
          &written,
          nullptr);
      if (succeeded == FALSE || written != block) {
        const DWORD native_code =
            succeeded == FALSE ? GetLastError() : ERROR_WRITE_FAULT;
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"イメージ未完了ファイル書込み",
            native_code));
      }
      completed += written;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    if (!handle_ || final_length == 0U || final_length > expected_length_ ||
        final_length > static_cast<std::uint64_t>(
                           (std::numeric_limits<LONGLONG>::max)())) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"イメージ未完了ファイル最終長",
          L"最終長が予約範囲外です");
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(final_length);
    if (!SetFilePointerEx(handle_.get(), end, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle_.get())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイル最終長の確定",
          GetLastError()));
    }
    expected_length_ = final_length;
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(
            handle_.get(), beginning, nullptr, FILE_BEGIN)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイル位置の再初期化",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) override {
    if (!handle_ || offset > expected_length_ ||
        length > expected_length_ - offset) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          staging_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"イメージ未完了ファイル読戻し範囲",
              L"読戻し範囲が作成予定長の外です"));
    }
    auto status = seek(offset, L"イメージ未完了ファイル読戻し位置");
    if (!status) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          status.error());
    }
    std::vector<std::byte> bytes(length);
    std::size_t completed = 0;
    while (completed < bytes.size()) {
      const DWORD block = static_cast<DWORD>(std::min<std::size_t>(
          bytes.size() - completed,
          std::numeric_limits<DWORD>::max()));
      DWORD read = 0;
      const BOOL succeeded = ReadFile(
          handle_.get(),
          bytes.data() + completed,
          block,
          &read,
          nullptr);
      if (succeeded == FALSE) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"イメージ未完了ファイル読戻し",
                GetLastError()));
      }
      if (read != block) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            staging_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_HANDLE_EOF,
                L"イメージ未完了ファイル読戻し",
                L"読戻し長が要求長と一致しません"));
      }
      completed += read;
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  [[nodiscard]] clonecore::Status flush() override {
    if (!handle_) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"イメージ未完了ファイルflush",
          L"未完了ファイルが開かれていません");
    }
    if (!FlushFileBuffers(handle_.get())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイルflush",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status close_file() override {
    if (!handle_) {
      return clonecore::success_status();
    }
    const HANDLE raw = handle_.release();
    if (!CloseHandle(raw)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイルclose",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status commit_no_replace(
      const std::wstring& partial_path,
      const std::wstring& final_path) override {
    if (!handle_ || !owns_partial_ ||
        !equals_ordinal_ignore_case(
            owned_partial_path_, partial_path)) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"イメージ完成ファイル確定",
          L"確定対象の未完了ファイルを所有していません");
    }
    const auto final_exists = path_exists(final_path);
    if (!final_exists) {
      return clonecore::Status::failure(final_exists.error());
    }
    if (final_exists.value()) {
      return staging_failure(
          clonecore::ErrorCode::confirmation_required,
          ERROR_FILE_EXISTS,
          L"イメージ完成ファイル非上書き確定",
          L"完成ファイルが既に存在するため上書きしません");
    }
    const std::size_t name_bytes =
        final_path.size() * sizeof(wchar_t);
    // FileNameLength excludes the terminator, but FILE_RENAME_INFO::FileName
    // is a NUL-terminated variable-length string. Keep one zero wchar_t in
    // the zero-initialized buffer after the copied absolute path.
    const std::size_t buffer_bytes =
        offsetof(FILE_RENAME_INFO, FileName) + name_bytes +
        sizeof(wchar_t);
    if (name_bytes > std::numeric_limits<DWORD>::max() ||
        buffer_bytes > std::numeric_limits<DWORD>::max()) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_FILENAME_EXCED_RANGE,
          L"イメージ完成ファイル非上書き確定",
          L"完成ファイル名がWindows API上限を超えています");
    }
    std::vector<std::byte> buffer(buffer_bytes);
    auto* rename =
        reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::copy(
        std::as_bytes(std::span(final_path)).begin(),
        std::as_bytes(std::span(final_path)).end(),
        buffer.begin() +
            static_cast<std::ptrdiff_t>(
                offsetof(FILE_RENAME_INFO, FileName)));
    if (!SetFileInformationByHandle(
            handle_.get(),
            FileRenameInfo,
            buffer.data(),
            static_cast<DWORD>(buffer.size()))) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ完成ファイルのハンドル非上書き確定",
          GetLastError()));
    }
    owns_partial_ = false;
    owned_partial_path_.clear();
    handle_.reset();
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status remove_owned_partial(
      const std::wstring& partial_path) override {
    if (!owns_partial_) {
      return clonecore::success_status();
    }
    if (!equals_ordinal_ignore_case(
            owned_partial_path_, partial_path)) {
      return staging_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_NAME,
          L"イメージ未完了ファイル破棄",
          L"所有中の未完了ファイルと破棄要求パスが一致しません");
    }
    if (handle_) {
      const auto closed = close_file();
      if (!closed) {
        return closed;
      }
    }
    if (!DeleteFileW(extended_path(partial_path).c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"イメージ未完了ファイル破棄",
          GetLastError()));
    }
    owns_partial_ = false;
    owned_partial_path_.clear();
    return clonecore::success_status();
  }

 private:
  clonecore::Status seek(
      const std::uint64_t offset,
      const std::wstring_view operation) {
    if (offset >
        static_cast<std::uint64_t>(
            std::numeric_limits<LONGLONG>::max())) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_ARITHMETIC_OVERFLOW,
          std::wstring(operation),
          L"ファイル位置が64bit符号付き範囲を超えています");
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(
            handle_.get(), position, nullptr, FILE_BEGIN)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          operation,
          GetLastError()));
    }
    return clonecore::success_status();
  }

  clonecore::UniqueHandle handle_;
  std::wstring owned_partial_path_;
  std::uint64_t expected_length_{};
  bool owns_partial_{};
};

class WindowsFileStagingTarget final : public IDcimgStagingTarget {
 public:
  WindowsFileStagingTarget(
      WindowsFileStagingRequest request,
      std::unique_ptr<IWindowsFileStagingBackend> backend)
      : request_(std::move(request)), backend_(std::move(backend)) {}

  ~WindowsFileStagingTarget() override {
    if (owns_partial_) {
      static_cast<void>(backend_->close_file());
      static_cast<void>(
          backend_->remove_owned_partial(initial_.partial_path));
    }
  }

  [[nodiscard]] clonecore::Status begin(
      const std::uint64_t expected_length) override {
    if (begun_ || committed_ || owns_partial_) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"イメージ段階ファイル開始",
          L"段階ファイルは一度だけ開始できます");
    }
    auto observed =
        backend_->observe_destination(request_.final_path);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    auto status = validate_initial_observation(
        request_, observed.value(), expected_length);
    if (!status) {
      return status;
    }
    initial_ = observed.take_value();
    expected_length_ = expected_length;
    status = backend_->create_new_restricted_partial(
        initial_.partial_path, expected_length);
    owns_partial_ = backend_->owns_partial();
    begun_ = owns_partial_;
    if (!status) {
      return status;
    }
    if (!owns_partial_) {
      return staging_failure(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"イメージ段階ファイル所有権確認",
          L"新規作成成功後に未完了ファイル所有権がありません");
    }

    auto reobserved =
        backend_->observe_destination(initial_.canonical_final_path);
    if (!reobserved) {
      return clonecore::Status::failure(reobserved.error());
    }
    return validate_active_observation(
        request_, initial_, reobserved.value());
  }

  [[nodiscard]] clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    const auto state = validate_active_range(
        offset, bytes.size(), L"イメージ段階ファイル書込み");
    if (!state) {
      return state;
    }
    return backend_->write_at(offset, bytes);
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    const auto state = validate_active_range(
        offset, length, L"イメージ段階ファイル読戻し");
    if (!state) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          state.error());
    }
    return backend_->read_at(offset, length);
  }

  [[nodiscard]] clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    auto state = validate_active(L"イメージ段階ファイル最終長");
    if (!state) {
      return state;
    }
    if (final_length == 0U || final_length > expected_length_) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"イメージ段階ファイル最終長",
          L"最終長が予約範囲外です");
    }
    state = backend_->resize_before_verification(final_length);
    if (!state) {
      return state;
    }
    expected_length_ = final_length;
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status flush() override {
    const auto state =
        validate_active(L"イメージ段階ファイルflush");
    if (!state) {
      return state;
    }
    return backend_->flush();
  }

  [[nodiscard]] clonecore::Status commit_verified() override {
    auto state = validate_active(L"イメージ段階ファイル確定");
    if (!state) {
      return state;
    }
    state = backend_->flush();
    if (!state) {
      return state;
    }
    auto observed =
        backend_->observe_destination(initial_.canonical_final_path);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    state = validate_active_observation(
        request_, initial_, observed.value());
    if (!state) {
      return state;
    }
    state = backend_->commit_no_replace(
        initial_.partial_path, initial_.canonical_final_path);
    owns_partial_ = backend_->owns_partial();
    if (!state) {
      return state;
    }
    if (owns_partial_) {
      return staging_failure(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"イメージ完成ファイル所有権確認",
          L"確定成功後も未完了ファイル所有状態が残っています");
    }
    committed_ = true;
    begun_ = false;
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status abort_incomplete() override {
    if (committed_) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"イメージ未完了ファイル破棄",
          L"確定済み完成ファイルは未完了破棄できません");
    }
    if (!owns_partial_) {
      begun_ = false;
      return clonecore::success_status();
    }
    const auto closed = backend_->close_file();
    const auto removed =
        backend_->remove_owned_partial(initial_.partial_path);
    owns_partial_ = backend_->owns_partial();
    if (!removed) {
      return removed;
    }
    if (!closed) {
      return closed;
    }
    begun_ = false;
    aborted_ = true;
    return clonecore::success_status();
  }

 private:
  clonecore::Status validate_active(
      const std::wstring_view operation) const {
    if (!begun_ || !owns_partial_ || committed_ || aborted_) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          std::wstring(operation),
          L"未完了ファイルが有効な作成状態ではありません");
    }
    return clonecore::success_status();
  }

  clonecore::Status validate_active_range(
      const std::uint64_t offset,
      const std::size_t length,
      const std::wstring_view operation) const {
    const auto state = validate_active(operation);
    if (!state) {
      return state;
    }
    if (offset > expected_length_ ||
        length > expected_length_ - offset) {
      return staging_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          std::wstring(operation),
          L"I/O範囲が作成予定長の外です");
    }
    return clonecore::success_status();
  }

  WindowsFileStagingRequest request_;
  std::unique_ptr<IWindowsFileStagingBackend> backend_;
  WindowsFileDestinationObservation initial_;
  std::uint64_t expected_length_{};
  bool begun_{};
  bool owns_partial_{};
  bool committed_{};
  bool aborted_{};
};

clonecore::Status validate_factory_request(
    const WindowsFileStagingRequest& request) {
  if (!is_drive_absolute_path(request.final_path) ||
      !has_dcimg_extension(request.final_path) ||
      has_forbidden_path_character(request.final_path)) {
    return staging_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"イメージ保存先要求検証",
        L"保存先はローカルドライブ上の絶対.dcimgパスで指定してください");
  }
  auto status =
      validate_identity_self(request.expected_source_disk, L"コピー元");
  if (!status) {
    return status;
  }
  if (request.expected_clone_target_disk.has_value()) {
    status = validate_identity_self(
        *request.expected_clone_target_disk, L"コピー先");
    if (!status) {
      return status;
    }
    if (same_stable_device(
            request.expected_source_disk,
            *request.expected_clone_target_disk)) {
      return staging_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DRIVE,
          L"コピー元とコピー先の分離",
          L"コピー元とコピー先が同じ安定識別情報です");
    }
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::unique_ptr<IDcimgStagingTarget>>
make_windows_file_staging_target(
    const WindowsFileStagingRequest& request) {
  return make_windows_file_staging_target_with_backend(
      request, std::make_unique<Win32FileStagingBackend>());
}

clonecore::Result<std::unique_ptr<IDcimgStagingTarget>>
make_windows_file_staging_target_with_backend(
    const WindowsFileStagingRequest& request,
    std::unique_ptr<IWindowsFileStagingBackend> backend) {
  const auto status = validate_factory_request(request);
  if (!status) {
    return clonecore::Result<
        std::unique_ptr<IDcimgStagingTarget>>::failure(status.error());
  }
  if (!backend) {
    return clonecore::Result<
        std::unique_ptr<IDcimgStagingTarget>>::failure(staging_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"イメージ段階ファイルBackend",
            L"Backendが指定されていません"));
  }
  return clonecore::Result<
      std::unique_ptr<IDcimgStagingTarget>>::success(
          std::make_unique<WindowsFileStagingTarget>(
              request, std::move(backend)));
}

}  // namespace ytec::imageformat

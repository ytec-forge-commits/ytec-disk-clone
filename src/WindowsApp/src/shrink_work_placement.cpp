#include "ytec/windowsapp/shrink_work_placement.h"

#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;

clonecore::Error placement_error(
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

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(placement_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool absolute_local_path(const std::wstring_view path) noexcept {
  const bool drive_path = path.size() >= 3U &&
      ((path[0] >= L'A' && path[0] <= L'Z') ||
       (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
  const bool extended_drive_path = path.size() >= 7U &&
      path.substr(0U, 4U) == L"\\\\?\\" &&
      ((path[4] >= L'A' && path[4] <= L'Z') ||
       (path[4] >= L'a' && path[4] <= L'z')) &&
      path[5] == L':' && (path[6] == L'\\' || path[6] == L'/');
  return drive_path || extended_drive_path;
}

std::wstring normalized_local_path_for_comparison(
    const std::wstring_view path) {
  std::wstring normalized(path);
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  if (normalized.size() >= 7U &&
      normalized.substr(0U, 4U) == L"\\\\?\\" &&
      normalized[5] == L':') {
    normalized.erase(0U, 4U);
  }
  while (normalized.size() > 3U && normalized.back() == L'\\') {
    normalized.pop_back();
  }
  return normalized;
}

bool same_local_path(
    const std::wstring_view requested,
    const std::wstring_view observed) {
  const auto left = normalized_local_path_for_comparison(requested);
  const auto right = normalized_local_path_for_comparison(observed);
  if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& requested,
    const std::wstring_view operation) {
  if (!absolute_local_path(requested) ||
      requested.size() >= kMaximumPathCharacters) {
    return clonecore::Result<std::wstring>::failure(placement_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ローカルドライブ上の絶対パスが必要です"));
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
            std::wstring(operation),
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::wstring canonical(buffer.data(), length);
  std::replace(canonical.begin(), canonical.end(), L'/', L'\\');
  if (!absolute_local_path(canonical)) {
    return clonecore::Result<std::wstring>::failure(placement_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"正規化後のパスがローカルドライブを指していません"));
  }
  return clonecore::Result<std::wstring>::success(std::move(canonical));
}

clonecore::Status verify_directory_chain(
    const std::filesystem::path& directory,
    const std::wstring_view operation) {
  if (!directory.is_absolute() ||
      directory.root_name().wstring().size() != 2U) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"作業パスの親ディレクトリが不正です");
  }
  std::filesystem::path current = directory.root_path();
  const auto check = [&](const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                   : ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"存在する通常ディレクトリだけを経由できます");
    }
    return clonecore::success_status();
  };
  auto status = check(current);
  if (!status) {
    return status;
  }
  for (const auto& component : directory.relative_path()) {
    if (component == L".") {
      continue;
    }
    if (component == L"..") {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          std::wstring(operation),
          L"親参照を含む作業パスは使用できません");
    }
    current /= component;
    status = check(current);
    if (!status) {
      return status;
    }
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint32_t> observe_path_disk_number(
    const std::wstring& canonical,
    const bool directory,
    const std::wstring_view operation) {
  const std::filesystem::path path(canonical);
  const std::filesystem::path parent = directory ? path : path.parent_path();
  auto status = verify_directory_chain(parent, operation);
  if (!status) {
    return clonecore::Result<std::uint32_t>::failure(status.error());
  }
  if (directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Result<std::uint32_t>::failure(placement_error(
          clonecore::ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                   : ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"scratchは存在する通常ディレクトリである必要があります"));
    }
  } else {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)) {
      return clonecore::Result<std::uint32_t>::failure(placement_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"作業ファイル位置が通常ファイルではありません"));
    }
  }
  const std::wstring probe = directory
      ? (path / L".ytec-shrink-placement-probe").wstring()
      : canonical;
  return diskmodel::query_single_disk_number_for_local_path(probe);
}

clonecore::Result<clonecore::StableDiskIdentity> identity_for_disk_number(
    const diskmodel::InventoryReport& inventory,
    const std::uint32_t disk_number) {
  const auto found = std::find_if(
      inventory.disks.begin(),
      inventory.disks.end(),
      [disk_number](const auto& disk) {
        return disk.disk_number == disk_number;
      });
  if (found == inventory.disks.end()) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        placement_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"縮小移行作業場所安定識別",
            L"作業場所の物理ディスクを現在の列挙へ対応付けできません"));
  }
  const auto duplicates = static_cast<std::size_t>(std::count_if(
      inventory.disks.begin(),
      inventory.disks.end(),
      [disk_number](const auto& disk) {
        return disk.disk_number == disk_number;
      }));
  if (duplicates != 1U) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        placement_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"縮小移行作業場所安定識別",
            L"同じディスク番号の候補が複数あります"));
  }
  return diskmodel::make_stable_disk_identity(*found, found->is_system_disk);
}

clonecore::Status validate_observed_path(
    const clonecore::StableDiskIdentity& protected_source,
    const std::wstring_view requested,
    const WindowsShrinkWorkPathObservation& observed,
    const std::wstring_view role) {
  if (requested.empty() || observed.canonical_path.empty() ||
      !absolute_local_path(requested) ||
      !absolute_local_path(observed.canonical_path) ||
      !observed.local_volume || observed.parent_is_reparse) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        std::wstring(role),
        L"作業場所はreparseを含まないローカルボリューム上の絶対パスである必要があります");
  }
  if (!same_local_path(requested, observed.canonical_path)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        std::wstring(role),
        L"依頼した作業パスとopened-volumeで正規化したパスが一致しません");
  }
  const auto observed_self = clonecore::validate_stable_identity(
      observed.backing_disk, observed.backing_disk, role);
  if (!observed_self) {
    return observed_self;
  }
  if (clonecore::validate_stable_identity(
          protected_source, observed.backing_disk, role)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        std::wstring(role),
        L"保護対象ディスク上にscratch、checkpoint、またはlogを配置できません");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Status validate_windows_shrink_work_placement_observation(
    const clonecore::StableDiskIdentity& protected_source,
    const WindowsShrinkWorkPaths& requested,
    const WindowsShrinkWorkPlacementObservation& observed) {
  const auto source_self = clonecore::validate_stable_identity(
      protected_source, protected_source, L"縮小移行保護対象");
  if (!source_self) {
    return source_self;
  }
  auto status = validate_observed_path(
      protected_source,
      requested.scratch_directory,
      observed.scratch,
      L"縮小移行scratch配置");
  if (!status) {
    return status;
  }
  status = validate_observed_path(
      protected_source,
      requested.checkpoint_path,
      observed.checkpoint,
      L"縮小移行checkpoint配置");
  if (!status) {
    return status;
  }
  if (requested.log_is_ram_only) {
    if (!requested.log_path.empty() || !observed.log.canonical_path.empty()) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"縮小移行RAM log配置",
          L"RAM限定ログではfilesystem上のlog pathを指定できません");
    }
    return clonecore::success_status();
  }
  return validate_observed_path(
      protected_source,
      requested.log_path,
      observed.log,
      L"縮小移行log配置");
}

clonecore::Result<WindowsShrinkWorkPlacementObservation>
observe_windows_shrink_work_placement_with_windows_apis(
    const WindowsShrinkWorkPaths& paths) {
  auto scratch = canonical_local_path(
      paths.scratch_directory, L"縮小移行scratch正規化");
  auto checkpoint = canonical_local_path(
      paths.checkpoint_path, L"縮小移行checkpoint正規化");
  if (!scratch || !checkpoint) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        scratch ? checkpoint.error() : scratch.error());
  }
  clonecore::Result<std::wstring> log = paths.log_is_ram_only
      ? clonecore::Result<std::wstring>::success({})
      : canonical_local_path(paths.log_path, L"縮小移行log正規化");
  if (!log || (paths.log_is_ram_only && !paths.log_path.empty())) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        log ? placement_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"縮小移行RAM log",
                  L"RAM限定ログにfilesystem pathを指定できません")
            : log.error());
  }

  auto scratch_before = observe_path_disk_number(
      scratch.value(), true, L"縮小移行scratch物理ディスク");
  auto checkpoint_before = observe_path_disk_number(
      checkpoint.value(), false, L"縮小移行checkpoint物理ディスク");
  clonecore::Result<std::uint32_t> log_before = paths.log_is_ram_only
      ? clonecore::Result<std::uint32_t>::success(0U)
      : observe_path_disk_number(
            log.value(), false, L"縮小移行log物理ディスク");
  if (!scratch_before || !checkpoint_before || !log_before) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        !scratch_before
            ? scratch_before.error()
            : !checkpoint_before ? checkpoint_before.error()
                                 : log_before.error());
  }

  auto provider = diskmodel::make_windows_disk_inventory_provider();
  if (!provider) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        placement_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"縮小移行作業場所ディスク列挙",
            L"ディスク一覧プロバイダーを作成できませんでした"));
  }
  auto inventory = provider->enumerate();
  if (!inventory) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        inventory.error());
  }
  auto scratch_after = observe_path_disk_number(
      scratch.value(), true, L"縮小移行scratch物理ディスク再確認");
  auto checkpoint_after = observe_path_disk_number(
      checkpoint.value(), false, L"縮小移行checkpoint物理ディスク再確認");
  clonecore::Result<std::uint32_t> log_after = paths.log_is_ram_only
      ? clonecore::Result<std::uint32_t>::success(0U)
      : observe_path_disk_number(
            log.value(), false, L"縮小移行log物理ディスク再確認");
  if (!scratch_after || !checkpoint_after || !log_after ||
      scratch_before.value() != scratch_after.value() ||
      checkpoint_before.value() != checkpoint_after.value() ||
      log_before.value() != log_after.value()) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        placement_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"縮小移行作業場所再識別",
            L"ディスク列挙の前後で作業場所の物理ディスクが変化しました"));
  }

  auto scratch_identity = identity_for_disk_number(
      inventory.value(), scratch_before.value());
  auto checkpoint_identity = identity_for_disk_number(
      inventory.value(), checkpoint_before.value());
  clonecore::Result<clonecore::StableDiskIdentity> log_identity =
      paths.log_is_ram_only
      ? clonecore::Result<clonecore::StableDiskIdentity>::success({})
      : identity_for_disk_number(inventory.value(), log_before.value());
  if (!scratch_identity || !checkpoint_identity || !log_identity) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        !scratch_identity
            ? scratch_identity.error()
            : !checkpoint_identity ? checkpoint_identity.error()
                                   : log_identity.error());
  }
  return clonecore::Result<WindowsShrinkWorkPlacementObservation>::success({
      .scratch = {
          .canonical_path = scratch.take_value(),
          .backing_disk = scratch_identity.take_value(),
          .local_volume = true,
      },
      .checkpoint = {
          .canonical_path = checkpoint.take_value(),
          .backing_disk = checkpoint_identity.take_value(),
          .local_volume = true,
      },
      .log = paths.log_is_ram_only
          ? WindowsShrinkWorkPathObservation{}
          : WindowsShrinkWorkPathObservation{
                .canonical_path = log.take_value(),
                .backing_disk = log_identity.take_value(),
                .local_volume = true,
            },
  });
}

}  // namespace ytec::windowsapp

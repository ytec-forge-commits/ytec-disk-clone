#include "ytec/migrationengine/volume_apply.h"

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/windowsdism/dism.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

clonecore::Error apply_error(
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
  return clonecore::Result<T>::failure(apply_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool equal_case_insensitive(
    const std::wstring& left,
    const std::wstring& right) noexcept {
  return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring decode_process_diagnostic(const std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const int source_length = static_cast<int>((std::min<std::size_t>)(
      value.size(), 1024U));
  const int required = MultiByteToWideChar(
      CP_OEMCP, 0, value.data(), source_length, nullptr, 0);
  if (required <= 0) {
    return {};
  }
  std::wstring decoded(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(
          CP_OEMCP,
          0,
          value.data(),
          source_length,
          decoded.data(),
          required) != required) {
    return {};
  }
  for (auto& character : decoded) {
    if (character == L'\r' || character == L'\n' || character == L'\t') {
      character = L' ';
    } else if (character < L' ') {
      character = L'?';
    }
  }
  while (!decoded.empty() && decoded.back() == L' ') {
    decoded.pop_back();
  }
  return decoded;
}

std::optional<std::wstring> choose_unused_drive_root() {
  const DWORD mask = GetLogicalDrives();
  if (mask == 0U) {
    return std::nullopt;
  }
  for (wchar_t letter = L'W'; letter >= L'D'; --letter) {
    if (letter == L'X') {
      continue;
    }
    const DWORD bit = 1U << static_cast<unsigned int>(letter - L'A');
    if ((mask & bit) == 0U) {
      return std::wstring{letter, L':', L'\\'};
    }
  }
  return std::nullopt;
}

class TemporaryVolumeMount final {
 public:
  TemporaryVolumeMount(std::wstring root, std::wstring volume)
      : root_(std::move(root)), volume_(std::move(volume)) {}

  ~TemporaryVolumeMount() {
    if (owned_mount_point_) {
      (void)DeleteVolumeMountPointW(root_.c_str());
    }
    if (owned_dos_device_) {
      (void)DefineDosDeviceW(
          DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION |
              DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM,
          drive_name_.c_str(),
          nt_device_path_.c_str());
    }
  }

  TemporaryVolumeMount(const TemporaryVolumeMount&) = delete;
  TemporaryVolumeMount& operator=(const TemporaryVolumeMount&) = delete;

  [[nodiscard]] clonecore::Status attach() {
    if (attached_) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小移行コピー先一時マウント",
          ERROR_ALREADY_EXISTS));
    }
    const DWORD drives = GetLogicalDrives();
    if (drives == 0U) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"縮小移行コピー先ドライブ列挙",
          GetLastError()));
    }
    std::optional<std::wstring> existing_root;
    for (wchar_t letter = L'D'; letter <= L'W'; ++letter) {
      if (letter == L'X') {
        continue;
      }
      const DWORD bit = 1U << static_cast<unsigned int>(letter - L'A');
      if ((drives & bit) == 0U) {
        continue;
      }
      const std::wstring candidate{letter, L':', L'\\'};
      std::array<wchar_t, MAX_PATH + 1U> candidate_volume{};
      if (GetVolumeNameForVolumeMountPointW(
              candidate.c_str(),
              candidate_volume.data(),
              static_cast<DWORD>(candidate_volume.size())) &&
          equal_case_insensitive(candidate_volume.data(), volume_)) {
        if (existing_root.has_value()) {
          return clonecore::Status::failure(apply_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              L"縮小移行コピー先既存ドライブ文字",
              L"同じコピー先Volumeへ複数のドライブ文字が対応しています"));
        }
        existing_root = candidate;
      }
    }
    if (existing_root.has_value()) {
      root_ = std::move(*existing_root);
      attached_ = true;
      return clonecore::success_status();
    }

    if (root_.size() != 3U || root_[1] != L':' || root_[2] != L'\\') {
      return clonecore::Status::failure(apply_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"縮小移行一時DOSデバイス",
          L"一時ドライブ文字が不正です"));
    }
    drive_name_ = root_.substr(0, 2);
    if (volume_.size() <= 5U || !volume_.starts_with(L"\\\\?\\") ||
        !volume_.ends_with(L'\\')) {
      return clonecore::Status::failure(apply_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_NAME,
          L"縮小移行一時DOSデバイス",
          L"Volume GUIDパスが正規形ではありません"));
    }
    const std::wstring volume_device_name =
        volume_.substr(4U, volume_.size() - 5U);
    std::vector<wchar_t> target(32U * 1024U, L'\0');
    const DWORD length = QueryDosDeviceW(
        volume_device_name.c_str(),
        target.data(),
        static_cast<DWORD>(target.size()));
    if (length == 0U || target[0] == L'\0') {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"縮小移行Volume NTデバイス取得",
          GetLastError()));
    }
    nt_device_path_ = target.data();

    std::fill(target.begin(), target.end(), L'\0');
    SetLastError(ERROR_SUCCESS);
    const DWORD existing_definition = QueryDosDeviceW(
        drive_name_.c_str(),
        target.data(),
        static_cast<DWORD>(target.size()));
    const DWORD existing_definition_error = GetLastError();
    if (existing_definition != 0U ||
        existing_definition_error != ERROR_FILE_NOT_FOUND) {
      return clonecore::Status::failure(apply_error(
          clonecore::ErrorCode::identity_mismatch,
          existing_definition != 0U
              ? ERROR_ALREADY_ASSIGNED
              : existing_definition_error,
          L"縮小移行一時DOSデバイス事前確認",
          L"選択した一時ドライブ文字に既存のDOSデバイス定義があります"));
    }
    if (!DefineDosDeviceW(
            DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM,
            drive_name_.c_str(),
            nt_device_path_.c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小移行コピー先一時マウント",
          GetLastError()));
    }
    owned_dos_device_ = true;

    std::array<wchar_t, MAX_PATH + 1U> resolved_volume{};
    if (!GetVolumeNameForVolumeMountPointW(
            root_.c_str(),
            resolved_volume.data(),
            static_cast<DWORD>(resolved_volume.size())) ||
        !equal_case_insensitive(resolved_volume.data(), volume_)) {
      const DWORD native_code = resolved_volume[0] == L'\0'
          ? GetLastError()
          : ERROR_INVALID_DATA;
      if (!DefineDosDeviceW(
          DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION |
              DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM,
          drive_name_.c_str(),
          nt_device_path_.c_str())) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"縮小移行一時DOSデバイス照合失敗後解除",
            GetLastError()));
      }
      owned_dos_device_ = false;
      return clonecore::Status::failure(apply_error(
          clonecore::ErrorCode::identity_mismatch,
          native_code,
          L"縮小移行一時DOSデバイス再確認",
          L"一時ドライブ文字が予定したVolume GUIDを指していません"));
    }

    if (!DefineDosDeviceW(
            DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION |
                DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM,
            drive_name_.c_str(),
            nt_device_path_.c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小移行一時DOSデバイス解除",
          GetLastError()));
    }
    owned_dos_device_ = false;

    std::fill(target.begin(), target.end(), L'\0');
    SetLastError(ERROR_SUCCESS);
    const DWORD removed_definition = QueryDosDeviceW(
        drive_name_.c_str(),
        target.data(),
        static_cast<DWORD>(target.size()));
    const DWORD removed_definition_error = GetLastError();
    if (removed_definition != 0U ||
        removed_definition_error != ERROR_FILE_NOT_FOUND) {
      return clonecore::Status::failure(apply_error(
          clonecore::ErrorCode::identity_mismatch,
          removed_definition != 0U
              ? ERROR_ALREADY_ASSIGNED
              : removed_definition_error,
          L"縮小移行一時DOSデバイス解除再確認",
          L"一時DOSデバイス定義が完全に解除されていません"));
    }

    if (!SetVolumeMountPointW(root_.c_str(), volume_.c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小移行コピー先マウントポイント登録",
          GetLastError()));
    }
    owned_mount_point_ = true;
    resolved_volume.fill(L'\0');
    if (!GetVolumeNameForVolumeMountPointW(
            root_.c_str(),
            resolved_volume.data(),
            static_cast<DWORD>(resolved_volume.size())) ||
        !equal_case_insensitive(resolved_volume.data(), volume_)) {
      const DWORD native_code = resolved_volume[0] == L'\0'
          ? GetLastError()
          : ERROR_INVALID_DATA;
      if (!DeleteVolumeMountPointW(root_.c_str())) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"縮小移行マウントポイント照合失敗後解除",
            GetLastError()));
      }
      owned_mount_point_ = false;
      return clonecore::Status::failure(apply_error(
          clonecore::ErrorCode::identity_mismatch,
          native_code,
          L"縮小移行マウントポイント再確認",
          L"登録したドライブ文字が予定したVolume GUIDを指していません"));
    }
    attached_ = true;
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status release() {
    if (!attached_) {
      return clonecore::success_status();
    }
    if (owned_mount_point_ && !DeleteVolumeMountPointW(root_.c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"縮小移行コピー先一時マウント解除",
          GetLastError()));
    }
    if (owned_mount_point_) {
      std::array<wchar_t, MAX_PATH + 1U> resolved_volume{};
      if (GetVolumeNameForVolumeMountPointW(
              root_.c_str(),
              resolved_volume.data(),
              static_cast<DWORD>(resolved_volume.size())) &&
          equal_case_insensitive(resolved_volume.data(), volume_)) {
        return clonecore::Status::failure(apply_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_ALREADY_ASSIGNED,
            L"縮小移行コピー先一時マウント解除再確認",
            L"コピー先Volumeの一時ドライブ文字が解除されていません"));
      }
      owned_mount_point_ = false;
    }
    attached_ = false;
    return clonecore::success_status();
  }

  [[nodiscard]] const std::wstring& root() const noexcept { return root_; }

 private:
  std::wstring root_;
  std::wstring volume_;
  std::wstring drive_name_;
  std::wstring nt_device_path_;
  bool attached_{};
  bool owned_mount_point_{};
  bool owned_dos_device_{};
};

clonecore::Result<std::wstring> format_executable(
    const std::wstring& trusted_system_directory) {
  if (trusted_system_directory.empty()) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"縮小移行FORMATパス",
        L"信頼済みSystem32がありません");
  }
  std::wstring path = trusted_system_directory;
  if (!path.ends_with(L'\\')) {
    path.push_back(L'\\');
  }
  path.append(L"format.com");
  return clonecore::Result<std::wstring>::success(std::move(path));
}

clonecore::Status execute_format(
    const std::wstring& drive_root,
    const migrationcore::MigrationFileSystem file_system,
    const std::uint64_t cluster_size,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust,
    bootrepair::IProcessRunner& process) {
  auto executable = format_executable(trusted_system_directory);
  auto arguments = build_format_arguments(drive_root, file_system, cluster_size);
  if (!executable) {
    return clonecore::Status::failure(executable.error());
  }
  if (!arguments) {
    return clonecore::Status::failure(arguments.error());
  }
  auto trusted = trust.verify_microsoft_signed(executable.value());
  if (!trusted) {
    return trusted;
  }
  const auto result = process.run(
      executable.value(), arguments.value(), trusted_system_directory);
  if (!result) {
    return clonecore::Status::failure(result.error());
  }
  trusted = trust.verify_microsoft_signed(executable.value());
  if (!trusted) {
    return trusted;
  }
  if (result.value().exit_code != 0U) {
    std::wstring message = L"Microsoft FORMATがエラーを返しました";
    const std::wstring standard_output =
        decode_process_diagnostic(result.value().standard_output);
    const std::wstring standard_error =
        decode_process_diagnostic(result.value().standard_error);
    if (!standard_output.empty()) {
      message.append(L": stdout=").append(standard_output);
    }
    if (!standard_error.empty()) {
      message.append(L": stderr=").append(standard_error);
    }
    return clonecore::Status::failure(apply_error(
        clonecore::ErrorCode::io_failed,
        result.value().exit_code,
        L"縮小移行コピー先フォーマット",
        std::move(message)));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<clonecore::VolumeBitmapBinding>>
query_target_bindings(
    const diskmodel::DiskInfo& target,
    const ShrinkTargetLayout& layout) {
  std::vector<diskmodel::VolumePartitionLocation> expected;
  for (const auto& partition : layout.migration.target_partitions) {
    if (partition.action ==
        migrationcore::MigrationPartitionAction::create_reserved) {
      continue;
    }
    expected.push_back(diskmodel::VolumePartitionLocation{
        .table_index = partition.target_number - 1U,
        .offset_bytes = partition.offset_bytes,
    });
  }
  return diskmodel::query_windows_volume_bindings_by_offset(target, expected);
}

clonecore::Result<std::vector<clonecore::VolumeBitmapBinding>>
query_target_bindings_with_arrival_retry(
    const diskmodel::DiskInfo& target,
    const ShrinkTargetLayout& layout,
    const clonecore::DiskOperationCallbacks& callbacks) {
  constexpr std::uint32_t attempts = 120U;
  std::optional<clonecore::Error> last_error;
  for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
    auto bindings = query_target_bindings(target, layout);
    if (bindings) {
      return bindings;
    }
    last_error = bindings.error();
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return failure<std::vector<clonecore::VolumeBitmapBinding>>(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小移行コピー先Volume待機",
          L"新規Volumeの到着待ち中に取り消されました");
    }
    Sleep(250U);
  }
  return clonecore::Result<
      std::vector<clonecore::VolumeBitmapBinding>>::failure(
      last_error.value_or(apply_error(
          clonecore::ErrorCode::query_failed,
          ERROR_TIMEOUT,
          L"縮小移行コピー先Volume待機",
          L"新規Volumeを30秒以内に一意に対応付けできませんでした")));
}

clonecore::Status verify_file_system(
    const std::wstring& root,
    const migrationcore::MigrationFileSystem expected) {
  std::array<wchar_t, 32> actual{};
  if (!GetVolumeInformationW(
          root.c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          actual.data(),
          static_cast<DWORD>(actual.size()))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::verification_failed,
        L"縮小移行フォーマット結果",
        GetLastError()));
  }
  const wchar_t* expected_name =
      expected == migrationcore::MigrationFileSystem::fat32
      ? L"FAT32"
      : L"NTFS";
  if (_wcsicmp(actual.data(), expected_name) != 0) {
    return clonecore::Status::failure(apply_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"縮小移行フォーマット結果",
        L"コピー先ファイルシステムが計画と一致しません"));
  }
  return clonecore::success_status();
}

std::uint64_t source_cluster_size(
    const VerifiedShrinkBundle& bundle,
    const migrationcore::ShrinkPlannedPartition& target) {
  if (!target.source_table_index.has_value()) {
    return 4096U;
  }
  const auto source = std::find_if(
      bundle.manifest.partitions.begin(),
      bundle.manifest.partitions.end(),
      [&](const auto& partition) {
        return partition.source_table_index == *target.source_table_index;
      });
  return source == bundle.manifest.partitions.end()
      ? 0U
      : source->cluster_size;
}

const VerifiedShrinkPayload* payload_for(
    const VerifiedShrinkBundle& bundle,
    const std::uint32_t source_table_index) {
  const auto partition = std::find_if(
      bundle.manifest.partitions.begin(),
      bundle.manifest.partitions.end(),
      [&](const auto& value) {
        return value.source_table_index == source_table_index;
      });
  if (partition == bundle.manifest.partitions.end()) {
    return nullptr;
  }
  const auto payload = std::find_if(
      bundle.payloads.begin(),
      bundle.payloads.end(),
      [&](const auto& value) {
        return value.file_name == partition->payload_file_name;
      });
  return payload == bundle.payloads.end() ? nullptr : &*payload;
}

}  // namespace

clonecore::Result<std::vector<std::wstring>> build_format_arguments(
    const std::wstring& format_target,
    const migrationcore::MigrationFileSystem file_system,
    const std::uint64_t cluster_size) {
  const bool drive_root = format_target.size() == 3U &&
      format_target[1] == L':' && format_target[2] == L'\\' &&
      std::iswalpha(format_target[0]) != 0;
  if (!drive_root ||
      (file_system != migrationcore::MigrationFileSystem::ntfs &&
       file_system != migrationcore::MigrationFileSystem::fat32) ||
      cluster_size < 512U || cluster_size > 2U * 1024U * 1024U ||
      (cluster_size & (cluster_size - 1U)) != 0U) {
    return failure<std::vector<std::wstring>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"縮小移行FORMAT引数",
        L"一時ドライブ、ファイルシステム、またはクラスター寸法が不正です");
  }
  return clonecore::Result<std::vector<std::wstring>>::success({
      format_target.substr(0, 2),
      file_system == migrationcore::MigrationFileSystem::fat32
          ? L"/FS:FAT32"
          : L"/FS:NTFS",
      L"/A:" + std::to_wstring(cluster_size),
      L"/Q",
      L"/Y",
  });
}

clonecore::Result<ShrinkVolumeApplyReport> format_and_apply_shrink_volumes(
    const diskmodel::DiskInfo& observed_target,
    const ShrinkTargetLayout& layout,
    const VerifiedShrinkBundle& bundle,
    const std::wstring& scratch_directory,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (observed_target.disk_number ==
          (std::numeric_limits<std::uint32_t>::max)() ||
      observed_target.size_bytes != layout.migration.target_size_bytes ||
      scratch_directory.empty()) {
    return failure<ShrinkVolumeApplyReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"縮小移行コピー先ボリューム",
        L"再識別コピー先と計画の寸法が一致しません");
  }
  auto bindings = query_target_bindings_with_arrival_retry(
      observed_target, layout, callbacks);
  if (!bindings) {
    return clonecore::Result<ShrinkVolumeApplyReport>::failure(
        bindings.error());
  }
  ShrinkVolumeApplyReport report{
      .every_volume_extent_reidentified = true,
  };
  for (const auto& partition : layout.migration.target_partitions) {
    if (partition.action ==
        migrationcore::MigrationPartitionAction::create_reserved) {
      continue;
    }
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return failure<ShrinkVolumeApplyReport>(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小移行ボリューム処理",
          L"ボリューム間の安全な境界で取り消しました");
    }
    const auto binding = std::find_if(
        bindings.value().begin(),
        bindings.value().end(),
        [&](const auto& value) {
          return value.partition_entry_index == partition.target_number - 1U;
        });
    if (binding == bindings.value().end()) {
      return failure<ShrinkVolumeApplyReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"縮小移行コピー先Volume GUID",
          L"新規パーティションのVolume GUIDを一意に特定できません");
    }
    const auto root = choose_unused_drive_root();
    if (!root.has_value()) {
      return failure<ShrinkVolumeApplyReport>(
          clonecore::ErrorCode::unsupported_platform,
          ERROR_NO_MORE_FILES,
          L"縮小移行一時ドライブ",
          L"安全に使用できる一時ドライブ文字がありません");
    }
    TemporaryVolumeMount mount(*root, binding->volume_device_path);
    auto status = mount.attach();
    if (!status) {
      return clonecore::Result<ShrinkVolumeApplyReport>::failure(status.error());
    }
    auto rebound = query_target_bindings(observed_target, layout);
    if (!rebound) {
      return clonecore::Result<ShrinkVolumeApplyReport>::failure(rebound.error());
    }
    const auto rebound_volume = std::find_if(
        rebound.value().begin(),
        rebound.value().end(),
        [&](const auto& value) {
          return value.partition_entry_index == partition.target_number - 1U;
        });
    if (rebound_volume == rebound.value().end() ||
        !equal_case_insensitive(
            rebound_volume->volume_device_path, binding->volume_device_path)) {
      return failure<ShrinkVolumeApplyReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"縮小移行コピー先再対応付け",
          L"一時マウント後にVolume GUIDと物理範囲の対応が変化しました");
    }
    const std::uint64_t cluster = source_cluster_size(bundle, partition);
    status = execute_format(
        mount.root(),
        partition.file_system,
        cluster,
        trusted_system_directory,
        trust_verifier,
        process_runner);
    if (!status) {
      return clonecore::Result<ShrinkVolumeApplyReport>::failure(status.error());
    }
    status = verify_file_system(mount.root(), partition.file_system);
    if (!status) {
      return clonecore::Result<ShrinkVolumeApplyReport>::failure(status.error());
    }
    if (!partition.label.empty() &&
        !SetVolumeLabelW(mount.root().c_str(), partition.label.c_str())) {
      return clonecore::Result<ShrinkVolumeApplyReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"縮小移行コピー先ラベル設定",
              GetLastError()));
    }
    ++report.formatted_volume_count;
    if (partition.action ==
        migrationcore::MigrationPartitionAction::apply_file_image) {
      if (!partition.source_table_index.has_value()) {
        return failure<ShrinkVolumeApplyReport>(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_DATA,
            L"縮小移行WIM対応",
            L"適用対象にコピー元パーティション番号がありません");
      }
      const auto* payload =
          payload_for(bundle, *partition.source_table_index);
      if (payload == nullptr) {
        return failure<ShrinkVolumeApplyReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_NOT_FOUND,
            L"縮小移行WIM対応",
            L"検証済みWIMを対象パーティションへ対応付けできません");
      }
      const auto applied = windowsdism::execute_dism_apply(
          windowsdism::DismApplyRequest{
              .image_path = payload->absolute_path,
              .target_root = mount.root(),
              .scratch_directory = scratch_directory,
          },
          trusted_system_directory,
          trust_verifier,
          process_runner);
      if (!applied) {
        return clonecore::Result<ShrinkVolumeApplyReport>::failure(
            applied.error());
      }
      ++report.applied_wim_count;
      report.applied_payload_bytes += payload->length_bytes;
    }
    status = mount.release();
    if (!status) {
      return clonecore::Result<ShrinkVolumeApplyReport>::failure(status.error());
    }
  }
  report.every_temporary_mount_released = true;
  return clonecore::Result<ShrinkVolumeApplyReport>::success(report);
}

}  // namespace ytec::migrationengine

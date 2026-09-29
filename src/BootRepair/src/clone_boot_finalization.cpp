#include "ytec/bootrepair/clone_boot_finalization.h"

#include "ytec/bootrepair/offline_windows.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kBasicDataPartitionType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::uint32_t kVolumeArrivalRetryCount = 120U;
constexpr DWORD kVolumeArrivalRetryDelayMilliseconds = 250U;

clonecore::Error finalization_error(
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

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool is_drive_root(const std::wstring_view value) {
  return value.size() == 3U && std::iswalpha(value[0]) != 0 &&
      value[1] == L':' &&
      (value[2] == L'\\' || value[2] == L'/');
}

std::wstring normalize_drive_root(std::wstring value) {
  if (is_drive_root(value)) {
    value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    value[2] = L'\\';
  }
  return value;
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes &&
      left.style == right.style && left.type == right.type &&
      left.identifier == right.identifier && left.name == right.name &&
      left.bootable == right.bootable;
}

bool same_partition_layout(
    const diskmodel::DiskInfo& left,
    const diskmodel::DiskInfo& right) {
  if (left.partition_style != right.partition_style ||
      left.partitions.size() != right.partitions.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.partitions.size(); ++index) {
    if (!same_partition(left.partitions[index], right.partitions[index])) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_partition_layout(
    const diskmodel::DiskInfo& disk,
    const diskmodel::PartitionStyle expected_style) {
  std::vector<const diskmodel::PartitionInfo*> ordered;
  ordered.reserve(disk.partitions.size());
  for (const auto& partition : disk.partitions) {
    if (partition.number == 0U || partition.size_bytes == 0U ||
        partition.style != expected_style ||
        partition.offset_bytes > disk.size_bytes ||
        partition.size_bytes > disk.size_bytes - partition.offset_bytes) {
      return clonecore::Status::failure(finalization_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"クローン後起動再構築のパーティション検証",
          L"対象レイアウトに未確定、範囲外、または形式不一致の区画があります"));
    }
    ordered.push_back(&partition);
  }
  std::sort(
      ordered.begin(), ordered.end(),
      [](const auto* left, const auto* right) {
        if (left->offset_bytes != right->offset_bytes) {
          return left->offset_bytes < right->offset_bytes;
        }
        return left->number < right->number;
      });
  for (std::size_t index = 1U; index < ordered.size(); ++index) {
    const auto* previous = ordered[index - 1U];
    const auto* current = ordered[index];
    if (previous->number == current->number ||
        previous->offset_bytes + previous->size_bytes >
            current->offset_bytes) {
      return clonecore::Status::failure(finalization_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"クローン後起動再構築のパーティション重複検証",
          L"対象レイアウトに区画番号または物理範囲の重複があります"));
    }
  }
  return clonecore::success_status();
}

clonecore::Result<diskmodel::DiskInfo> resolve_target(
    const clonecore::StableDiskIdentity& expected,
    const diskmodel::InventoryReport& inventory,
    const std::wstring_view operation) {
  if (!inventory.issues.empty()) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(
        finalization_error(
            clonecore::ErrorCode::query_failed,
            ERROR_INVALID_DATA,
            std::wstring(operation) + L"の全ディスク再列挙",
            L"未解決の列挙診断があるため対象を確定できません"));
  }
  std::vector<const diskmodel::DiskInfo*> matches;
  for (const auto& disk : inventory.disks) {
    const auto identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (identity && clonecore::validate_stable_identity(
                        expected, identity.value(), operation)) {
      matches.push_back(&disk);
    }
  }
  if (matches.size() != 1U) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(
        finalization_error(
            clonecore::ErrorCode::identity_mismatch,
            matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
            std::wstring(operation) + L"の安定再識別",
            L"確認済みの物理ディスクを一意に再識別できません"));
  }
  return clonecore::Result<diskmodel::DiskInfo>::success(*matches.front());
}

clonecore::Status validate_online_target(
    const diskmodel::DiskInfo& target,
    const diskmodel::PartitionStyle expected_style) {
  if ((expected_style != diskmodel::PartitionStyle::gpt &&
       expected_style != diskmodel::PartitionStyle::mbr) ||
      target.partition_style != expected_style || target.partitions.empty() ||
      target.is_system_disk || !target.offline.has_value() ||
      target.offline.value() || !target.read_only.has_value() ||
      target.read_only.value() || !target.removable.has_value() ||
      target.removable.value()) {
    return clonecore::Status::failure(finalization_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_READY,
        L"クローン後起動再構築の対象状態",
        L"形式一致・オンライン・書込み可能・固定の非システム対象だけを処理できます"));
  }
  return validate_partition_layout(target, expected_style);
}

bool same_location(
    const BootRepairVolumeLocation& location,
    const diskmodel::DiskInfo& disk,
    const diskmodel::PartitionInfo& partition) {
  return location.disk_number == disk.disk_number &&
      location.starting_offset == partition.offset_bytes &&
      location.extent_length == partition.size_bytes;
}

clonecore::Result<const BootVolumeObservation*> exact_volume_for_partition(
    const diskmodel::DiskInfo& target,
    const diskmodel::PartitionInfo& partition,
    const std::vector<BootVolumeObservation>& volumes,
    const std::wstring_view role) {
  std::vector<const BootVolumeObservation*> matches;
  for (const auto& volume : volumes) {
    if (same_location(volume.location, target, partition)) {
      matches.push_back(&volume);
    }
  }
  if (matches.size() != 1U) {
    return clonecore::Result<const BootVolumeObservation*>::failure(
        finalization_error(
            clonecore::ErrorCode::identity_mismatch,
            matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
            std::wstring(role) + L"のVolume GUID対応",
            L"対象パーティション範囲に一致するVolume GUIDを一意に確認できません"));
  }
  return clonecore::Result<const BootVolumeObservation*>::success(
      matches.front());
}

struct LocatedWindows final {
  diskmodel::PartitionInfo partition;
  BootVolumeObservation volume;
};

struct LocatedCloneVolumes final {
  diskmodel::DiskInfo target;
  std::vector<BootVolumeObservation> volumes;
  LocatedWindows windows;
  diskmodel::PartitionInfo system_partition;
  BootVolumeObservation system_volume;
};

clonecore::Result<LocatedWindows> locate_windows(
    const CloneBootFinalizationRequest& request,
    const diskmodel::DiskInfo& target,
    const std::vector<BootVolumeObservation>& volumes,
    ICloneBootFinalizationVolumeProvider& volume_provider) {
  std::vector<LocatedWindows> candidates;
  for (const auto& partition : target.partitions) {
    if (request.expected_windows_partition_offset.has_value() &&
        partition.offset_bytes !=
            request.expected_windows_partition_offset.value()) {
      continue;
    }
    if (request.expected_style == diskmodel::PartitionStyle::gpt &&
        !equals_case_insensitive(partition.type, kBasicDataPartitionType)) {
      continue;
    }
    const auto volume = exact_volume_for_partition(
        target, partition, volumes, L"Windows領域候補");
    if (!volume) {
      // Partitions without a mounted filesystem are not Windows candidates.
      // An explicitly requested offset, however, must map exactly.
      if (request.expected_windows_partition_offset.has_value() ||
          volume.error().native_code != ERROR_NOT_FOUND) {
        return clonecore::Result<LocatedWindows>::failure(volume.error());
      }
      continue;
    }
    if (!equals_case_insensitive(volume.value()->location.file_system, L"NTFS")) {
      if (request.expected_windows_partition_offset.has_value()) {
        return clonecore::Result<LocatedWindows>::failure(
            finalization_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"Windows領域候補のファイルシステム検証",
                L"指定されたWindows領域候補はNTFSではありません"));
      }
      continue;
    }
    const auto supported =
        volume_provider.contains_supported_offline_windows(
            volume.value()->volume_name);
    if (!supported) {
      return clonecore::Result<LocatedWindows>::failure(supported.error());
    }
    if (supported.value()) {
      candidates.push_back(LocatedWindows{
          .partition = partition,
          .volume = *volume.value(),
      });
    }
  }
  if (candidates.size() != 1U) {
    return clonecore::Result<LocatedWindows>::failure(finalization_error(
        clonecore::ErrorCode::identity_mismatch,
        candidates.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"クローン後Windows領域の一意選択",
        L"対応するWindows 10/11 x64領域を一つだけ特定できません"));
  }
  return clonecore::Result<LocatedWindows>::success(
      std::move(candidates.front()));
}

clonecore::Result<diskmodel::PartitionInfo> locate_system_partition(
    const diskmodel::DiskInfo& target,
    const BcdBootFirmware firmware) {
  std::vector<const diskmodel::PartitionInfo*> matches;
  for (const auto& partition : target.partitions) {
    const bool match = firmware == BcdBootFirmware::uefi
        ? partition.style == diskmodel::PartitionStyle::gpt &&
            equals_case_insensitive(partition.type, kEfiPartitionType)
        : partition.style == diskmodel::PartitionStyle::mbr &&
            partition.bootable &&
            equals_case_insensitive(partition.type, L"0x07");
    if (match) {
      matches.push_back(&partition);
    }
  }
  if (matches.size() != 1U) {
    return clonecore::Result<diskmodel::PartitionInfo>::failure(
        finalization_error(
            clonecore::ErrorCode::identity_mismatch,
            matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
            L"クローン後システム領域の一意選択",
            L"ESPまたはBIOS Active NTFS領域を一つだけ特定できません"));
  }
  return clonecore::Result<diskmodel::PartitionInfo>::success(
      *matches.front());
}

clonecore::Result<LocatedCloneVolumes> wait_for_cloned_volumes(
    const CloneBootFinalizationRequest& request,
    const diskmodel::DiskInfo& initial_target,
    const BcdBootFirmware firmware,
    diskmodel::IDiskInventoryProvider& inventory,
    ICloneBootFinalizationVolumeProvider& volume_provider) {
  auto target = initial_target;
  for (std::uint32_t retry = 0U;; ++retry) {
    std::optional<clonecore::Error> pending_error;
    auto volumes = volume_provider.observe_volumes_read_only();
    if (!volumes) {
      if (volumes.error().native_code != ERROR_NOT_FOUND) {
        return clonecore::Result<LocatedCloneVolumes>::failure(
            volumes.error());
      }
      pending_error = volumes.error();
    } else {
      auto windows = locate_windows(
          request, target, volumes.value(), volume_provider);
      if (!windows) {
        if (windows.error().native_code != ERROR_NOT_FOUND) {
          return clonecore::Result<LocatedCloneVolumes>::failure(
              windows.error());
        }
        pending_error = windows.error();
      } else {
        auto system_partition = locate_system_partition(target, firmware);
        if (!system_partition) {
          return clonecore::Result<LocatedCloneVolumes>::failure(
              system_partition.error());
        }
        auto system_volume = exact_volume_for_partition(
            target,
            system_partition.value(),
            volumes.value(),
            L"クローン後システム領域");
        if (!system_volume) {
          if (system_volume.error().native_code != ERROR_NOT_FOUND) {
            return clonecore::Result<LocatedCloneVolumes>::failure(
                system_volume.error());
          }
          pending_error = system_volume.error();
        } else {
          const std::wstring_view expected_system_file_system =
              firmware == BcdBootFirmware::uefi ? L"FAT32" : L"NTFS";
          if (!equals_case_insensitive(
                  system_volume.value()->location.file_system,
                  expected_system_file_system)) {
            return clonecore::Result<LocatedCloneVolumes>::failure(
                finalization_error(
                    clonecore::ErrorCode::unsupported_layout,
                    ERROR_NOT_SUPPORTED,
                    L"クローン後システム領域のファイルシステム検証",
                    L"ESPはFAT32、BIOS Active領域はNTFSである必要があります"));
          }
          auto system_volume_observation = *system_volume.value();
          return clonecore::Result<LocatedCloneVolumes>::success({
              .target = std::move(target),
              .volumes = volumes.take_value(),
              .windows = windows.take_value(),
              .system_partition = system_partition.take_value(),
              .system_volume = std::move(system_volume_observation),
          });
        }
      }
    }

    if (!pending_error.has_value()) {
      return clonecore::Result<LocatedCloneVolumes>::failure(
          finalization_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_STATE,
              L"クローン後ボリューム到着待機",
              L"再試行条件を確定できませんでした"));
    }
    if (retry >= kVolumeArrivalRetryCount) {
      return clonecore::Result<LocatedCloneVolumes>::failure(
          finalization_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_TIMEOUT,
              L"クローン後ボリューム到着待機",
              L"コピー先をオンラインにした後、Windows領域またはシステム領域の認識が30秒以内に完了しませんでした"));
    }

    const auto waited = volume_provider.wait_before_volume_retry();
    if (!waited) {
      return clonecore::Result<LocatedCloneVolumes>::failure(waited.error());
    }
    const auto refreshed_inventory = inventory.enumerate();
    if (!refreshed_inventory) {
      return clonecore::Result<LocatedCloneVolumes>::failure(
          refreshed_inventory.error());
    }
    auto refreshed_target = resolve_target(
        request.expected_target,
        refreshed_inventory.value(),
        L"クローン後ボリューム到着待機対象");
    if (!refreshed_target) {
      return clonecore::Result<LocatedCloneVolumes>::failure(
          refreshed_target.error());
    }
    const auto refreshed_status =
        validate_online_target(refreshed_target.value(), request.expected_style);
    if (!refreshed_status) {
      return clonecore::Result<LocatedCloneVolumes>::failure(
          refreshed_status.error());
    }
    if (!same_partition_layout(initial_target, refreshed_target.value())) {
      return clonecore::Result<LocatedCloneVolumes>::failure(
          finalization_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"クローン後ボリューム到着待機中のレイアウト再識別",
              L"待機中に対象パーティション構成が変化しました"));
    }
    target = refreshed_target.take_value();
  }
}

clonecore::Result<std::optional<std::wstring>> existing_root(
    const BootVolumeObservation& volume,
    const std::wstring_view role) {
  if (volume.mount_points.empty()) {
    return clonecore::Result<std::optional<std::wstring>>::success(
        std::nullopt);
  }
  if (volume.mount_points.size() != 1U ||
      !is_drive_root(volume.mount_points.front())) {
    return clonecore::Result<std::optional<std::wstring>>::failure(
        finalization_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_ALREADY_ASSIGNED,
            std::wstring(role) + L"の既存マウント検証",
            L"対象Volume GUIDに単一のドライブ文字以外の割当があります"));
  }
  return clonecore::Result<std::optional<std::wstring>>::success(
      normalize_drive_root(volume.mount_points.front()));
}

clonecore::Result<std::wstring> select_unused_root(
    std::wstring& unavailable) {
  std::array<bool, 26> used{};
  for (const wchar_t character : unavailable) {
    if (std::iswalpha(character) != 0) {
      const wchar_t upper = static_cast<wchar_t>(std::towupper(character));
      used[static_cast<std::size_t>(upper - L'A')] = true;
    }
  }
  used[static_cast<std::size_t>(L'X' - L'A')] = true;
  for (wchar_t letter = L'Y'; letter >= L'D'; --letter) {
    if (!used[static_cast<std::size_t>(letter - L'A')]) {
      unavailable.push_back(letter);
      return clonecore::Result<std::wstring>::success(
          std::wstring(1U, letter) + L":\\");
    }
  }
  return clonecore::Result<std::wstring>::failure(finalization_error(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NO_MORE_ITEMS,
      L"クローン後起動再構築の一時ドライブ文字選択",
      L"安全に使用できる未使用ドライブ文字がありません"));
}

clonecore::Result<TemporarySystemVolumeMount> acquire_exact_mount(
    const diskmodel::DiskInfo& target,
    const diskmodel::PartitionInfo& partition,
    const BootVolumeObservation& volume,
    const BcdBootFirmware firmware,
    std::wstring& unavailable,
    ISystemVolumeMountApi& mount_api) {
  const auto root = select_unused_root(unavailable);
  if (!root) {
    return clonecore::Result<TemporarySystemVolumeMount>::failure(
        root.error());
  }
  return TemporarySystemVolumeMount::acquire(
      TemporarySystemVolumeMountPlan{
          .firmware = firmware,
          .disk_number = target.disk_number,
          .partition_number = partition.number,
          .volume_name = volume.volume_name,
          .temporary_root = root.value(),
          .expected_location = volume.location,
      },
      mount_api);
}

clonecore::Status validate_repair_selection(
    const BootRepairTargetSelection& selection,
    const clonecore::StableDiskIdentity& expected_identity,
    const diskmodel::PartitionInfo& expected_windows,
    const diskmodel::PartitionInfo& expected_system,
    const std::wstring_view operation) {
  const auto identity = clonecore::validate_stable_identity(
      expected_identity, selection.identity, operation);
  if (!identity) {
    return identity;
  }
  if (!same_partition(selection.windows_partition, expected_windows) ||
      !same_partition(selection.system_partition, expected_system)) {
    return clonecore::Status::failure(finalization_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        std::wstring(operation),
        L"起動修復境界が選択したWindowsまたはシステム領域が計画と一致しません"));
  }
  return clonecore::success_status();
}

struct CloneEfiOwnershipBinding final {
  std::wstring system_volume_identity_root;
  EfiBootOwnershipEvidence evidence;
};

clonecore::Result<CloneEfiOwnershipBinding>
inspect_clone_efi_ownership(
    const BcdBootFirmware firmware,
    const BootVolumeObservation& system_volume,
    IEfiBootOwnershipInspector& inspector) {
  if (firmware != BcdBootFirmware::uefi) {
    return clonecore::Result<CloneEfiOwnershipBinding>::success(
        CloneEfiOwnershipBinding{});
  }

  const auto identity_root =
      normalize_offline_windows_volume_root(system_volume.volume_name);
  if (!identity_root || is_drive_root(system_volume.volume_name)) {
    return clonecore::Result<CloneEfiOwnershipBinding>::failure(
        identity_root
            ? finalization_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_NAME,
                  L"クローン後ESP Volume GUID識別",
                  L"UEFI起動再構築には厳密なESP Volume GUIDルートが必要です")
            : identity_root.error());
  }

  auto ownership = inspector.inspect_existing_esp_read_only(
      identity_root.value());
  if (!ownership) {
    return clonecore::Result<CloneEfiOwnershipBinding>::failure(
        ownership.error());
  }
  if (ownership.value().state !=
          EfiBootOwnershipState::microsoft_only_or_empty ||
      !efi_boot_ownership_allows_microsoft_rebuild(ownership.value())) {
    return clonecore::Result<CloneEfiOwnershipBinding>::failure(
        finalization_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"クローン後ESP EFI所有権診断",
            L"クローン先ESPが空またはMicrosoft所有と一意に確認できないため起動再構築を開始しません"));
  }
  return clonecore::Result<CloneEfiOwnershipBinding>::success({
      .system_volume_identity_root = identity_root.value(),
      .evidence = ownership.take_value(),
  });
}

class WindowsCloneBootFinalizationVolumeProvider final
    : public ICloneBootFinalizationVolumeProvider {
 public:
  clonecore::Result<std::vector<BootVolumeObservation>>
  observe_volumes_read_only() override {
    return enumerate_windows_boot_volumes_read_only();
  }

  clonecore::Status wait_before_volume_retry() override {
    Sleep(kVolumeArrivalRetryDelayMilliseconds);
    return clonecore::success_status();
  }

  clonecore::Result<std::wstring> unavailable_drive_letters() override {
    const DWORD mask = GetLogicalDrives();
    if (mask == 0U) {
      return clonecore::Result<std::wstring>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"クローン後起動再構築のドライブ文字列挙",
              GetLastError()));
    }
    std::wstring letters;
    for (std::uint32_t index = 0U; index < 26U; ++index) {
      if ((mask & (1UL << index)) != 0U) {
        letters.push_back(static_cast<wchar_t>(L'A' + index));
      }
    }
    return clonecore::Result<std::wstring>::success(std::move(letters));
  }

  clonecore::Result<bool> contains_supported_offline_windows(
      const std::wstring& volume_root) override {
    const auto normalized = normalize_offline_windows_volume_root(volume_root);
    if (!normalized) {
      return clonecore::Result<bool>::failure(normalized.error());
    }
    const std::wstring kernel =
        normalized.value() + L"Windows\\System32\\ntoskrnl.exe";
    const DWORD attributes = GetFileAttributesW(kernel.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return clonecore::Result<bool>::success(false);
      }
      return clonecore::Result<bool>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"クローン後Windowsカーネル候補確認",
              native_code));
    }
    if ((attributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
      return clonecore::Result<bool>::failure(finalization_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          L"クローン後Windowsカーネル候補検証",
          L"通常ファイルではないWindowsカーネル候補を拒否しました"));
    }
    const auto verified = verify_offline_windows_amd64(normalized.value());
    if (!verified) {
      return clonecore::Result<bool>::failure(verified.error());
    }
    return clonecore::Result<bool>::success(true);
  }
};

class WindowsCloneBootFinalizationService final
    : public ICloneBootFinalizationService {
 public:
  explicit WindowsCloneBootFinalizationService(
      diskmodel::IDiskInventoryProvider& inventory)
      : inventory_(inventory),
        volume_provider_(
            std::make_unique<WindowsCloneBootFinalizationVolumeProvider>()),
        efi_ownership_inspector_(
            make_windows_efi_boot_ownership_inspector()),
        mount_api_(make_windows_system_volume_mount_api()),
        boot_repair_service_(
            make_windows_standalone_boot_repair_service(inventory)) {}

  clonecore::Result<CloneBootFinalizationReport> execute(
      const CloneBootFinalizationRequest& request) override {
    if (volume_provider_ == nullptr ||
        efi_ownership_inspector_ == nullptr || mount_api_ == nullptr ||
        boot_repair_service_ == nullptr) {
      return clonecore::Result<CloneBootFinalizationReport>::failure(
          finalization_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_STATE,
              L"クローン後起動再構築サービス初期化",
              L"必要なWindows APIサービスを初期化できませんでした"));
    }
    return finalize_cloned_windows_boot(
        request,
        inventory_,
        *volume_provider_,
        *efi_ownership_inspector_,
        *mount_api_,
        *boot_repair_service_);
  }

 private:
  diskmodel::IDiskInventoryProvider& inventory_;
  std::unique_ptr<ICloneBootFinalizationVolumeProvider> volume_provider_;
  std::unique_ptr<IEfiBootOwnershipInspector> efi_ownership_inspector_;
  std::unique_ptr<ISystemVolumeMountApi> mount_api_;
  std::unique_ptr<IStandaloneBootRepairService> boot_repair_service_;
};

}  // namespace

clonecore::Result<CloneBootFinalizationReport>
finalize_cloned_windows_boot(
    const CloneBootFinalizationRequest& request,
    diskmodel::IDiskInventoryProvider& inventory,
    ICloneBootFinalizationVolumeProvider& volume_provider,
    IEfiBootOwnershipInspector& efi_ownership_inspector,
    ISystemVolumeMountApi& mount_api,
    IStandaloneBootRepairService& boot_repair_service) {
  if (request.expected_style != diskmodel::PartitionStyle::gpt &&
      request.expected_style != diskmodel::PartitionStyle::mbr) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        finalization_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"クローン後起動再構築の形式指定",
            L"GPTまたはMBRを明示する必要があります"));
  }

  const auto initial_inventory = inventory.enumerate();
  if (!initial_inventory) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        initial_inventory.error());
  }
  const auto initial_target = resolve_target(
      request.expected_target,
      initial_inventory.value(),
      L"クローン後起動再構築対象");
  if (!initial_target) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        initial_target.error());
  }
  const auto target_status =
      validate_online_target(initial_target.value(), request.expected_style);
  if (!target_status) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        target_status.error());
  }

  const BcdBootFirmware firmware =
      request.expected_style == diskmodel::PartitionStyle::gpt
      ? BcdBootFirmware::uefi
      : BcdBootFirmware::bios;
  auto located = wait_for_cloned_volumes(
      request,
      initial_target.value(),
      firmware,
      inventory,
      volume_provider);
  if (!located) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        located.error());
  }
  const auto& target = located.value().target;
  const auto& volumes = located.value().volumes;
  const auto& windows = located.value().windows;
  const auto& system_partition = located.value().system_partition;
  const auto& system_volume = located.value().system_volume;

  auto efi_ownership_binding = inspect_clone_efi_ownership(
      firmware, system_volume, efi_ownership_inspector);
  if (!efi_ownership_binding) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        efi_ownership_binding.error());
  }

  auto unavailable = volume_provider.unavailable_drive_letters();
  if (!unavailable) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        unavailable.error());
  }
  for (const auto& volume : volumes) {
    for (const auto& mount_point : volume.mount_points) {
      if (is_drive_root(mount_point)) {
        unavailable.value().push_back(mount_point[0]);
      }
    }
  }

  std::optional<TemporarySystemVolumeMount> windows_mount;
  std::optional<TemporarySystemVolumeMount> system_mount;
  auto windows_root = existing_root(windows.volume, L"Windows領域");
  if (!windows_root) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        windows_root.error());
  }
  if (!windows_root.value().has_value()) {
    auto mounted = acquire_exact_mount(
        target,
        windows.partition,
        windows.volume,
        firmware,
        unavailable.value(),
        mount_api);
    if (!mounted) {
      return clonecore::Result<CloneBootFinalizationReport>::failure(
          mounted.error());
    }
    windows_mount.emplace(mounted.take_value());
    windows_root.value() = windows_mount->root();
  }

  const bool same_system_partition =
      same_partition(windows.partition, system_partition);
  std::optional<std::wstring> system_root;
  if (same_system_partition) {
    if (firmware != BcdBootFirmware::bios) {
      const auto windows_release = windows_mount.has_value()
          ? windows_mount->release()
          : clonecore::success_status();
      return clonecore::Result<CloneBootFinalizationReport>::failure(
          windows_release
              ? finalization_error(
                    clonecore::ErrorCode::unsupported_layout,
                    ERROR_INVALID_DATA,
                    L"UEFIシステム領域とWindows領域の分離",
                    L"UEFIではESPとWindows領域を分離する必要があります")
              : windows_release.error());
    }
    system_root = windows_root.value();
  } else {
    auto observed_root = existing_root(
        system_volume, L"システム領域");
    if (!observed_root) {
      const auto windows_release = windows_mount.has_value()
          ? windows_mount->release()
          : clonecore::success_status();
      return clonecore::Result<CloneBootFinalizationReport>::failure(
          windows_release ? observed_root.error() : windows_release.error());
    }
    if (!observed_root.value().has_value()) {
      auto mounted = acquire_exact_mount(
          target,
          system_partition,
          system_volume,
          firmware,
          unavailable.value(),
          mount_api);
      if (!mounted) {
        const auto windows_release = windows_mount.has_value()
            ? windows_mount->release()
            : clonecore::success_status();
        return clonecore::Result<CloneBootFinalizationReport>::failure(
            windows_release ? mounted.error() : windows_release.error());
      }
      system_mount.emplace(mounted.take_value());
      observed_root.value() = system_mount->root();
    }
    system_root = observed_root.value();
  }

  const bool windows_temporarily_mounted = windows_mount.has_value();
  const bool system_temporarily_mounted = system_mount.has_value();
  const auto release_all = [&]() -> clonecore::Status {
    const auto system_release = system_mount.has_value()
        ? system_mount->release()
        : clonecore::success_status();
    const auto windows_release = windows_mount.has_value()
        ? windows_mount->release()
        : clonecore::success_status();
    return system_release ? windows_release : system_release;
  };
  const auto fail_after_cleanup = [&](const clonecore::Error& error) {
    const auto cleanup = release_all();
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        cleanup ? error : cleanup.error());
  };

  const BootRepairTargetRequest boot_request{
      .disk_number = target.disk_number,
      .windows_root = windows_root.value().value(),
      .system_root = system_root.value(),
      .firmware = firmware,
      .store_policy = BcdBootStorePolicy::rebuild_fresh,
      .auto_mount_system_partition = false,
      .system_volume_identity_root =
          std::move(
              efi_ownership_binding.value().system_volume_identity_root),
      .require_efi_ownership_recheck =
          firmware == BcdBootFirmware::uefi,
      .expected_efi_ownership =
          std::move(efi_ownership_binding.value().evidence),
      .third_party_efi_policy =
          BootRepairThirdPartyEfiPolicy::not_applicable,
      .reviewed_multi_windows_batch = false,
      .update_current_pc_nvram = false,
  };
  auto selection = boot_repair_service.inspect(boot_request);
  if (!selection) {
    return fail_after_cleanup(selection.error());
  }
  const auto selection_status = validate_repair_selection(
      selection.value(),
      request.expected_target,
      windows.partition,
      system_partition,
      L"BCDBoot直前の対象再識別");
  if (!selection_status) {
    return fail_after_cleanup(selection_status.error());
  }

  auto repaired = boot_repair_service.execute(
      StandaloneBootRepairExecutionRequest{
          .target = boot_request,
          .expected = selection.value(),
          .confirmation = clonecore::TargetConfirmation{
              .first_step_acknowledged = true,
              .typed_token = make_boot_repair_confirmation_token(
                  selection.value().identity, firmware),
          },
      });
  if (!repaired) {
    return fail_after_cleanup(repaired.error());
  }
  const auto repaired_selection_status = validate_repair_selection(
      repaired.value().repaired,
      request.expected_target,
      windows.partition,
      system_partition,
      L"BCDBoot後の対象再識別");
  if (!repaired_selection_status) {
    return fail_after_cleanup(repaired_selection_status.error());
  }
  if (!repaired.value().bcdboot.microsoft_signature_verified ||
      repaired.value().bcdboot.exit_code != 0U ||
      !repaired.value().bcdboot.fresh_store_verified ||
      !repaired.value().boot_store_verified ||
      repaired.value().system_partition_temporarily_mounted ||
      repaired.value().temporary_mount_released ||
      !repaired.value().efi_ownership_revalidated ||
      !repaired.value().nvram_unchanged) {
    return fail_after_cleanup(finalization_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"クローン後の新規BCD再構築トランザクション",
        L"Microsoft署名、BCDBoot /c、新規BCD読戻し、EFI所有権再検証、NVRAM不変、または限定マウント境界を確認できません"));
  }

  const auto cleanup = release_all();
  if (!cleanup) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        cleanup.error());
  }

  const auto final_inventory = inventory.enumerate();
  if (!final_inventory) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        final_inventory.error());
  }
  const auto final_target = resolve_target(
      request.expected_target,
      final_inventory.value(),
      L"クローン後起動再構築完了対象");
  if (!final_target) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        final_target.error());
  }
  const auto final_status =
      validate_online_target(final_target.value(), request.expected_style);
  if (!final_status) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        final_status.error());
  }
  if (!same_partition_layout(initial_target.value(), final_target.value())) {
    return clonecore::Result<CloneBootFinalizationReport>::failure(
        finalization_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"クローン後起動再構築の最終レイアウト再識別",
            L"起動再構築の前後で対象パーティション構成が変化しました"));
  }

  return clonecore::Result<CloneBootFinalizationReport>::success(
      CloneBootFinalizationReport{
          .boot_repair = repaired.take_value(),
          .windows_partition_temporarily_mounted =
              windows_temporarily_mounted,
          .system_partition_temporarily_mounted =
              system_temporarily_mounted,
          .temporary_mounts_released = true,
          .final_target_reidentified = true,
          .partition_layout_unchanged = true,
      });
}

std::unique_ptr<ICloneBootFinalizationService>
make_windows_clone_boot_finalization_service(
    diskmodel::IDiskInventoryProvider& inventory) {
  return std::make_unique<WindowsCloneBootFinalizationService>(inventory);
}

}  // namespace ytec::bootrepair

#include "ytec/bootrepair/standalone_repair.h"

#include "ytec/bootrepair/offline_windows.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::bootrepair {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kBasicDataPartitionType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";

clonecore::Error repair_error(
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

bool is_drive_root(const std::wstring& value) {
  return value.size() == 3 && std::iswalpha(value[0]) != 0 &&
         value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

std::wstring normalized_root(std::wstring value) {
  if (is_drive_root(value)) {
    value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    value[2] = L'\\';
  }
  return value;
}

bool equals_case_insensitive(
    const std::wstring& left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
         _wcsicmp(left.c_str(), right.data()) == 0;
}

clonecore::Result<BootRepairVolumeLocation> query_volume_location(
    const std::wstring& root) {
  const auto normalized_result =
      normalize_offline_windows_volume_root(root);
  if (!normalized_result) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(repair_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"起動修復対象ボリューム",
        L"ドライブ文字または厳密なVolume GUIDのルートを指定してください"));
  }
  const std::wstring normalized = normalized_result.value();
  const std::wstring device = is_drive_root(normalized)
      ? L"\\\\.\\" + normalized.substr(0, 2)
      : normalized.substr(0, normalized.size() - 1U);
  clonecore::UniqueHandle volume(CreateFileW(
      device.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"起動修復対象ボリュームを読取り専用で開く",
            GetLastError()));
  }

  std::array<std::byte, 1024> buffer{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr)) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"起動修復対象ボリュームのディスク対応取得",
            GetLastError()));
  }
  constexpr std::size_t kRequiredBytes =
      offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT);
  const auto* const extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (bytes_returned < kRequiredBytes ||
      extents->NumberOfDiskExtents != 1 ||
      extents->Extents[0].StartingOffset.QuadPart < 0 ||
      extents->Extents[0].ExtentLength.QuadPart <= 0) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(repair_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"起動修復対象ボリュームのディスク対応検証",
        L"単一物理ディスク上の通常パーティションだけを使用できます"));
  }

  std::array<wchar_t, MAX_PATH> file_system{};
  if (!GetVolumeInformationW(
          normalized.c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"起動修復対象ファイルシステム取得",
            GetLastError()));
  }
  return clonecore::Result<BootRepairVolumeLocation>::success(
      BootRepairVolumeLocation{
          .disk_number = extents->Extents[0].DiskNumber,
          .starting_offset = static_cast<std::uint64_t>(
              extents->Extents[0].StartingOffset.QuadPart),
          .extent_length = static_cast<std::uint64_t>(
              extents->Extents[0].ExtentLength.QuadPart),
          .file_system = file_system.data(),
      });
}

clonecore::Status verify_regular_file(
    const std::wstring& path,
    std::wstring operation,
    std::wstring missing_message) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::invalid_data,
        attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                               : ERROR_REPARSE_TAG_INVALID,
        std::move(operation),
        std::move(missing_message)));
  }
  return clonecore::success_status();
}

bool is_administrator() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(
          &authority,
          2,
          SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS,
          0,
          0,
          0,
          0,
          0,
          0,
          &administrators)) {
    return false;
  }
  BOOL member = FALSE;
  const BOOL checked = CheckTokenMembership(nullptr, administrators, &member);
  FreeSid(administrators);
  return checked != FALSE && member != FALSE;
}

bool same_partition(
    const diskmodel::PartitionInfo& expected,
    const diskmodel::PartitionInfo& observed) {
  return expected.number == observed.number &&
         expected.offset_bytes == observed.offset_bytes &&
         expected.size_bytes == observed.size_bytes &&
         expected.style == observed.style &&
         expected.type == observed.type &&
         expected.identifier == observed.identifier &&
         expected.name == observed.name &&
         expected.bootable == observed.bootable;
}

bool same_disk_layout_and_identity_fields(
    const diskmodel::DiskInfo& expected,
    const diskmodel::DiskInfo& observed) {
  return expected.disk_number == observed.disk_number &&
      expected.device_path == observed.device_path &&
      expected.device_instance_id == observed.device_instance_id &&
      expected.model == observed.model &&
      expected.size_bytes == observed.size_bytes &&
      expected.sector_count == observed.sector_count &&
      expected.logical_sector_size == observed.logical_sector_size &&
      expected.physical_sector_size == observed.physical_sector_size &&
      expected.bus_type == observed.bus_type &&
      expected.serial_suffix == observed.serial_suffix &&
      expected.partition_style == observed.partition_style &&
      expected.offline == observed.offline &&
      expected.read_only == observed.read_only &&
      expected.removable == observed.removable &&
      expected.is_system_disk == observed.is_system_disk &&
      expected.partitions.size() == observed.partitions.size() &&
      std::equal(
          expected.partitions.begin(),
          expected.partitions.end(),
          observed.partitions.begin(),
          same_partition);
}

clonecore::Status verify_boot_store(
    const std::wstring& system_root,
    const BcdBootFirmware firmware) {
  const std::wstring root = normalized_root(system_root);
  const std::wstring store_path = firmware == BcdBootFirmware::uefi
      ? root + L"EFI\\Microsoft\\Boot\\BCD"
      : root + L"Boot\\BCD";
  return verify_regular_file(
      store_path,
      L"再構築後BCDストア検証",
      L"BCDBoot成功後に通常ファイルのBCDストアを確認できません");
}

std::wstring used_drive_letters() {
  const DWORD mask = GetLogicalDrives();
  std::wstring letters;
  if (mask == 0U) {
    return letters;
  }
  for (std::uint32_t index = 0U; index < 26U; ++index) {
    if ((mask & (1U << index)) != 0U) {
      letters.push_back(static_cast<wchar_t>(L'A' + index));
    }
  }
  return letters;
}

struct InspectedBootRepairTarget final {
  BootRepairTargetSelection selection;
  BootRepairVolumeLocation system_volume;
  EfiBootOwnershipEvidence efi_ownership;
  std::optional<TemporarySystemVolumeMountPlan> temporary_mount;
};

bool same_volume_location(
    const BootRepairVolumeLocation& left,
    const BootRepairVolumeLocation& right) {
  return left.disk_number == right.disk_number &&
      left.starting_offset == right.starting_offset &&
      left.extent_length == right.extent_length &&
      equals_case_insensitive(left.file_system, right.file_system);
}

bool same_temporary_mount_plan(
    const std::optional<TemporarySystemVolumeMountPlan>& left,
    const std::optional<TemporarySystemVolumeMountPlan>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  return left->firmware == right->firmware &&
      left->disk_number == right->disk_number &&
      left->partition_number == right->partition_number &&
      equals_case_insensitive(left->volume_name, right->volume_name) &&
      equals_case_insensitive(left->temporary_root, right->temporary_root) &&
      same_volume_location(
          left->expected_location, right->expected_location);
}

bool same_multi_system_policy(
    const BootRepairTargetRequest& left,
    const BootRepairTargetRequest& right) {
  return left.disk_number == right.disk_number &&
      equals_case_insensitive(left.system_root, right.system_root) &&
      left.firmware == right.firmware &&
      left.auto_mount_system_partition ==
          right.auto_mount_system_partition &&
      equals_case_insensitive(
          left.system_volume_identity_root,
          right.system_volume_identity_root) &&
      left.require_efi_ownership_recheck ==
          right.require_efi_ownership_recheck &&
      equivalent_efi_boot_ownership(
          left.expected_efi_ownership,
          right.expected_efi_ownership) &&
      left.third_party_efi_policy == right.third_party_efi_policy &&
      left.reviewed_multi_windows_batch ==
          right.reviewed_multi_windows_batch &&
      left.update_current_pc_nvram == right.update_current_pc_nvram;
}

class WindowsStandaloneBootRepairService final
    : public IStandaloneBootRepairService {
 public:
  explicit WindowsStandaloneBootRepairService(
      diskmodel::IDiskInventoryProvider& inventory)
      : inventory_(inventory),
        mount_api_(make_windows_system_volume_mount_api()),
        efi_ownership_inspector_(
            make_windows_efi_boot_ownership_inspector()) {}

  clonecore::Result<BootRepairTargetSelection> inspect(
      const BootRepairTargetRequest& request) override {
    auto inspected = inspect_target(request);
    if (!inspected) {
      return clonecore::Result<BootRepairTargetSelection>::failure(
          inspected.error());
    }
    return clonecore::Result<BootRepairTargetSelection>::success(
        std::move(inspected.value().selection));
  }

  clonecore::Result<StandaloneBootRepairReport> execute(
      const StandaloneBootRepairExecutionRequest& request) override {
    if (!is_administrator()) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          repair_error(
              clonecore::ErrorCode::access_denied,
              ERROR_ELEVATION_REQUIRED,
              L"単独起動修復の管理者権限確認",
              L"起動ファイルを変更するにはWinPEまたは管理者権限が必要です"));
    }
    if (request.target.reviewed_multi_windows_batch) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          repair_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"レビュー済み複数Windows起動修復経路",
              L"レビュー済みバッチ構成員は単独トランザクションとして実行できません"));
    }
    auto inspected = inspect_target(request.target);
    if (!inspected) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          inspected.error());
    }
    auto& observed = inspected.value();
    const clonecore::Status selection_status =
        validate_boot_repair_selection(
            request.expected,
            observed.selection,
            request.target.firmware,
            request.confirmation);
    if (!selection_status) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          selection_status.error());
    }

    std::optional<TemporarySystemVolumeMount> temporary_mount;
    std::wstring system_root = normalized_root(request.target.system_root);
    if (observed.temporary_mount.has_value()) {
      if (mount_api_ == nullptr) {
        return clonecore::Result<StandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_STATE,
                L"一時システム領域のAPI初期化",
                L"一時割り当てAPIを初期化できませんでした"));
      }
      auto mounted = TemporarySystemVolumeMount::acquire(
          observed.temporary_mount.value(), *mount_api_);
      if (!mounted) {
        return clonecore::Result<StandaloneBootRepairReport>::failure(
            mounted.error());
      }
      temporary_mount.emplace(mounted.take_value());
      system_root = temporary_mount->root();
    }

    auto final_efi_ownership = inspect_efi_ownership(
        request.target, observed.system_volume);
    if (!final_efi_ownership) {
      if (temporary_mount.has_value()) {
        const clonecore::Status release_status = temporary_mount->release();
        if (!release_status) {
          return clonecore::Result<StandaloneBootRepairReport>::failure(
              release_status.error());
        }
      }
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          final_efi_ownership.error());
    }

    auto bcdboot = execute_bcdboot_with_windows_apis(BcdBootRequest{
        .target_windows_directory =
            normalized_root(request.target.windows_root) + L"Windows",
        .target_system_partition_root = system_root,
        .firmware = request.target.firmware,
        .store_policy = request.target.store_policy,
    });
    clonecore::Status store_status = clonecore::success_status();
    if (bcdboot) {
      store_status = verify_boot_store(system_root, request.target.firmware);
    }
    if (temporary_mount.has_value()) {
      const clonecore::Status release_status = temporary_mount->release();
      if (!release_status) {
        return clonecore::Result<StandaloneBootRepairReport>::failure(
            release_status.error());
      }
    }
    if (!bcdboot) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          bcdboot.error());
    }
    if (!store_status) {
      return clonecore::Result<StandaloneBootRepairReport>::failure(
          store_status.error());
    }
    return clonecore::Result<StandaloneBootRepairReport>::success(
        StandaloneBootRepairReport{
            .repaired = std::move(observed.selection),
            .bcdboot = bcdboot.take_value(),
            .boot_store_verified = true,
            .system_partition_temporarily_mounted =
                observed.temporary_mount.has_value(),
            .temporary_mount_released =
                observed.temporary_mount.has_value(),
            .efi_ownership_revalidated =
                request.target.firmware != BcdBootFirmware::uefi ||
                efi_boot_ownership_allows_microsoft_rebuild(
                    final_efi_ownership.value()),
            .nvram_unchanged = true,
        });
  }

  clonecore::Result<MultiWindowsStandaloneBootRepairReport>
  execute_multi_windows(
      const MultiWindowsStandaloneBootRepairExecutionRequest& request)
      override {
    constexpr std::size_t kMaximumWindowsInstallations = 32U;
    if (!is_administrator()) {
      return clonecore::Result<
          MultiWindowsStandaloneBootRepairReport>::failure(
          repair_error(
              clonecore::ErrorCode::access_denied,
              ERROR_ELEVATION_REQUIRED,
              L"複数Windows起動修復の管理者権限確認",
              L"起動ファイルを変更するにはWinPEまたは管理者権限が必要です"));
    }
    if (request.targets_in_boot_priority.empty() ||
        request.targets_in_boot_priority.size() >
            kMaximumWindowsInstallations ||
        request.targets_in_boot_priority.size() !=
            request.expected_in_boot_priority.size()) {
      return clonecore::Result<
          MultiWindowsStandaloneBootRepairReport>::failure(
          repair_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"複数Windows起動修復件数",
              L"1件以上32件以下の対象と同数のレビュー済み選択が必要です"));
    }

    std::vector<InspectedBootRepairTarget> observed;
    observed.reserve(request.targets_in_boot_priority.size());
    for (std::size_t index = 0U;
         index < request.targets_in_boot_priority.size(); ++index) {
      const auto& target = request.targets_in_boot_priority[index];
      if ((index == 0U &&
           target.store_policy != BcdBootStorePolicy::rebuild_fresh) ||
          (index != 0U &&
           target.store_policy != BcdBootStorePolicy::preserve_existing)) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_PARAMETER,
                L"複数Windows起動修復BCD方針",
                L"最初だけ新規再構築し、2件目以降は同じBCDへ追加する必要があります"));
      }
      if (!target.reviewed_multi_windows_batch) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::confirmation_required,
                ERROR_INVALID_STATE,
                L"複数Windows起動修復レビュー境界",
                L"pure reviewに束縛されたバッチ要求だけを実行できます"));
      }
      if (index != 0U && !same_multi_system_policy(
                             request.targets_in_boot_priority.front(),
                             target)) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_NOT_CONNECTED,
                L"複数Windows起動修復システム領域",
                L"すべてのWindowsが同じ対象ディスク、システム領域、EFI方針を使用していません"));
      }
      auto inspected = inspect_target(target);
      if (!inspected) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            inspected.error());
      }
      const clonecore::Status selection =
          validate_boot_repair_selection(
              request.expected_in_boot_priority[index],
              inspected.value().selection,
              target.firmware,
              request.confirmation);
      if (!selection) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            selection.error());
      }
      if (!observed.empty() &&
          (!clonecore::validate_stable_identity(
              observed.front().selection.identity,
              inspected.value().selection.identity,
              L"複数Windows起動修復対象") ||
           !same_partition(
               observed.front().selection.system_partition,
               inspected.value().selection.system_partition) ||
           !same_disk_layout_and_identity_fields(
               observed.front().selection.disk,
               inspected.value().selection.disk) ||
           !same_volume_location(
               observed.front().system_volume,
               inspected.value().system_volume) ||
           !same_temporary_mount_plan(
               observed.front().temporary_mount,
               inspected.value().temporary_mount) ||
           !equivalent_efi_boot_ownership(
               observed.front().efi_ownership,
               inspected.value().efi_ownership))) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_NOT_CONNECTED,
                L"複数Windows起動修復の直前再識別",
                L"Windows間で対象ディスクまたはシステム領域の再診断結果が一致しません"));
      }
      observed.push_back(inspected.take_value());
    }

    std::optional<TemporarySystemVolumeMount> temporary_mount;
    std::wstring system_root = normalized_root(
        request.targets_in_boot_priority.front().system_root);
    if (observed.front().temporary_mount.has_value()) {
      if (mount_api_ == nullptr) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            repair_error(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_STATE,
                L"複数Windows一時システム領域API",
                L"一時割り当てAPIを初期化できませんでした"));
      }
      auto mounted = TemporarySystemVolumeMount::acquire(
          observed.front().temporary_mount.value(), *mount_api_);
      if (!mounted) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            mounted.error());
      }
      temporary_mount.emplace(mounted.take_value());
      system_root = temporary_mount->root();
    }

    const auto& first_target =
        request.targets_in_boot_priority.front();
    auto final_efi_ownership = inspect_efi_ownership(
        first_target, observed.front().system_volume);
    if (!final_efi_ownership) {
      if (temporary_mount.has_value()) {
        const clonecore::Status released = temporary_mount->release();
        if (!released) {
          return clonecore::Result<
              MultiWindowsStandaloneBootRepairReport>::failure(
              released.error());
        }
      }
      return clonecore::Result<
          MultiWindowsStandaloneBootRepairReport>::failure(
          final_efi_ownership.error());
    }

    std::vector<BcdBootRequest> bcd_requests;
    bcd_requests.reserve(observed.size());
    for (std::size_t index = 0U; index < observed.size(); ++index) {
      bcd_requests.push_back(BcdBootRequest{
          .target_windows_directory = normalized_root(
              request.targets_in_boot_priority[index].windows_root) +
              L"Windows",
          .target_system_partition_root = system_root,
          .firmware = first_target.firmware,
          .store_policy = index == 0U
              ? BcdBootStorePolicy::rebuild_fresh
              : BcdBootStorePolicy::preserve_existing,
      });
    }
    auto bcdboot = execute_multi_windows_bcdboot_with_windows_apis(
        bcd_requests);
    clonecore::Status store_status = clonecore::success_status();
    if (bcdboot) {
      store_status = verify_boot_store(system_root, first_target.firmware);
    }
    if (temporary_mount.has_value()) {
      const clonecore::Status released = temporary_mount->release();
      if (!released) {
        return clonecore::Result<
            MultiWindowsStandaloneBootRepairReport>::failure(
            released.error());
      }
    }
    if (!bcdboot) {
      return clonecore::Result<
          MultiWindowsStandaloneBootRepairReport>::failure(
          bcdboot.error());
    }
    if (!store_status) {
      return clonecore::Result<
          MultiWindowsStandaloneBootRepairReport>::failure(
          store_status.error());
    }

    std::vector<BootRepairTargetSelection> repaired;
    repaired.reserve(observed.size());
    for (auto& item : observed) {
      repaired.push_back(std::move(item.selection));
    }
    return clonecore::Result<
        MultiWindowsStandaloneBootRepairReport>::success(
        MultiWindowsStandaloneBootRepairReport{
            .repaired_in_boot_priority = std::move(repaired),
            .bcdboot = bcdboot.take_value(),
            .boot_store_verified = true,
            .system_partition_temporarily_mounted =
                temporary_mount.has_value(),
            .temporary_mount_released = temporary_mount.has_value(),
            .efi_ownership_revalidated =
                first_target.firmware != BcdBootFirmware::uefi ||
                equivalent_efi_boot_ownership(
                    first_target.expected_efi_ownership,
                    final_efi_ownership.value()),
            .nvram_unchanged = true,
        });
  }

 private:
  clonecore::Result<InspectedBootRepairTarget> inspect_target(
      const BootRepairTargetRequest& request) {
    auto inventory = inventory_.enumerate();
    if (!inventory) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          inventory.error());
    }
    const auto windows_volume = query_volume_location(request.windows_root);
    if (!windows_volume) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          windows_volume.error());
    }
    BootRepairVolumeLocation system_volume;
    std::optional<TemporarySystemVolumeMountPlan> temporary_plan;
    if (request.auto_mount_system_partition) {
      if (!request.system_root.empty()) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            repair_error(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_PARAMETER,
                L"未割当システム領域の指定",
                L"自動検出時はシステム領域のドライブ文字を同時指定できません"));
      }
      const auto disk = std::find_if(
          inventory.value().disks.begin(),
          inventory.value().disks.end(),
          [&](const auto& candidate) {
            return candidate.disk_number == request.disk_number;
          });
      if (disk == inventory.value().disks.end()) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            repair_error(
                clonecore::ErrorCode::invalid_argument,
                ERROR_NOT_FOUND,
                L"未割当システム領域の対象ディスク選択",
                L"指定された物理ディスクが見つかりません"));
      }
      auto volumes = enumerate_windows_boot_volumes_read_only();
      if (!volumes) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            volumes.error());
      }
      auto plan = plan_temporary_system_volume_mount(
          *disk,
          request.firmware,
          volumes.value(),
          used_drive_letters() + request.windows_root);
      if (!plan) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            plan.error());
      }
      system_volume = plan.value().expected_location;
      temporary_plan = plan.take_value();
    } else {
      const auto queried_system_volume =
          query_volume_location(request.system_root);
      if (!queried_system_volume) {
        return clonecore::Result<InspectedBootRepairTarget>::failure(
            queried_system_volume.error());
      }
      system_volume = queried_system_volume.value();
    }
    auto selection = evaluate_boot_repair_target(
        request,
        inventory.value(),
        windows_volume.value(),
        system_volume);
    if (!selection) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          selection.error());
    }

    auto efi_ownership = inspect_efi_ownership(request, system_volume);
    if (!efi_ownership) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          efi_ownership.error());
    }

    const std::wstring windows_root = normalized_root(request.windows_root);
    const clonecore::Status architecture =
        verify_offline_windows_amd64(windows_root);
    if (!architecture) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          architecture.error());
    }
    const std::wstring loader =
        windows_root + L"Windows\\System32\\" +
        (request.firmware == BcdBootFirmware::uefi
             ? L"winload.efi"
             : L"winload.exe");
    const clonecore::Status loader_status = verify_regular_file(
        loader,
        L"起動修復対象Windowsブートローダー確認",
        L"対象Windowsに通常ファイルのブートローダーを確認できません");
    if (!loader_status) {
      return clonecore::Result<InspectedBootRepairTarget>::failure(
          loader_status.error());
    }
    return clonecore::Result<InspectedBootRepairTarget>::success(
        InspectedBootRepairTarget{
            .selection = selection.take_value(),
            .system_volume = system_volume,
            .efi_ownership = efi_ownership.take_value(),
            .temporary_mount = std::move(temporary_plan),
        });
  }

  clonecore::Result<EfiBootOwnershipEvidence> inspect_efi_ownership(
      const BootRepairTargetRequest& request,
      const BootRepairVolumeLocation& selected_system_volume) {
    if (request.firmware != BcdBootFirmware::uefi) {
      EfiBootOwnershipEvidence not_applicable;
      const clonecore::Status policy = validate_boot_repair_efi_ownership(
          request, not_applicable);
      if (!policy) {
        return clonecore::Result<EfiBootOwnershipEvidence>::failure(
            policy.error());
      }
      return clonecore::Result<EfiBootOwnershipEvidence>::success(
          not_applicable);
    }

    const auto normalized = normalize_offline_windows_volume_root(
        request.system_volume_identity_root);
    if (!normalized || is_drive_root(request.system_volume_identity_root)) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          normalized
              ? repair_error(
                    clonecore::ErrorCode::invalid_argument,
                    ERROR_INVALID_NAME,
                    L"UEFI ESP Volume GUID識別",
                    L"UEFI実行にはドライブ文字ではなく厳密なVolume GUIDルートが必要です")
              : normalized.error());
    }
    const auto identity_volume = query_volume_location(normalized.value());
    if (!identity_volume) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          identity_volume.error());
    }
    if (identity_volume.value().disk_number !=
            selected_system_volume.disk_number ||
        identity_volume.value().starting_offset !=
            selected_system_volume.starting_offset ||
        identity_volume.value().extent_length !=
            selected_system_volume.extent_length ||
        !equals_case_insensitive(
            identity_volume.value().file_system,
            selected_system_volume.file_system)) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          repair_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"UEFI ESP Volume GUID再識別",
              L"確認済みESPとEFI所有権診断用Volume GUIDの範囲が一致しません"));
    }
    if (efi_ownership_inspector_ == nullptr) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          repair_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_STATE,
              L"UEFI ESP所有権診断初期化",
              L"読取り専用EFI所有権診断を初期化できません"));
    }
    auto observed =
        efi_ownership_inspector_->inspect_existing_esp_read_only(
            normalized.value());
    if (!observed) {
      return observed;
    }
    const clonecore::Status policy = validate_boot_repair_efi_ownership(
        request, observed.value());
    if (!policy) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          policy.error());
    }
    return observed;
  }

  diskmodel::IDiskInventoryProvider& inventory_;
  std::unique_ptr<ISystemVolumeMountApi> mount_api_;
  std::unique_ptr<IEfiBootOwnershipInspector> efi_ownership_inspector_;
};

}  // namespace

clonecore::Result<MultiWindowsStandaloneBootRepairReport>
IStandaloneBootRepairService::execute_multi_windows(
    const MultiWindowsStandaloneBootRepairExecutionRequest&) {
  return clonecore::Result<
      MultiWindowsStandaloneBootRepairReport>::failure(
      repair_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"複数Windows起動修復サービス",
          L"この起動修復サービスは複数Windowsの一括登録に対応していません"));
}

clonecore::Result<BootRepairTargetSelection> evaluate_boot_repair_target(
    const BootRepairTargetRequest& request,
    const diskmodel::InventoryReport& inventory,
    const BootRepairVolumeLocation& windows_volume,
    const BootRepairVolumeLocation& system_volume) {
  const std::wstring windows_root = normalized_root(request.windows_root);
  const std::wstring system_root = normalized_root(request.system_root);
  const bool valid_system_target = request.auto_mount_system_partition
      ? system_root.empty()
      : is_drive_root(system_root);
  if (!is_drive_root(windows_root) || !valid_system_target ||
      (request.firmware != BcdBootFirmware::uefi &&
       request.firmware != BcdBootFirmware::bios) ||
      (request.firmware == BcdBootFirmware::uefi &&
       !request.auto_mount_system_partition &&
       windows_root[0] == system_root[0])) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"単独起動修復の対象指定",
        L"UEFI/BIOSとWindows/システム領域のドライブ文字指定が不正です"));
  }
  if (!inventory.issues.empty()) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"単独起動修復の全ディスク列挙",
        L"未解決の列挙診断があるため対象を確定できません"));
  }
  const auto disk = std::find_if(
      inventory.disks.begin(),
      inventory.disks.end(),
      [&](const auto& candidate) {
        return candidate.disk_number == request.disk_number;
      });
  if (disk == inventory.disks.end()) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"単独起動修復対象ディスクの選択",
        L"指定された物理ディスクが見つかりません"));
  }
  const diskmodel::PartitionStyle expected_style =
      request.firmware == BcdBootFirmware::uefi
      ? diskmodel::PartitionStyle::gpt
      : diskmodel::PartitionStyle::mbr;
  if (disk->partition_style != expected_style || disk->partitions.empty() ||
      disk->is_system_disk || !disk->read_only.has_value() ||
      !disk->removable.has_value() || disk->read_only.value() ||
      disk->removable.value()) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"単独起動修復対象ディスクの安全属性",
        L"オフラインの固定ディスクで、指定ファームウェアに対応するGPT/MBR構成だけを修復できます"));
  }
  if (windows_volume.disk_number != request.disk_number ||
      system_volume.disk_number != request.disk_number ||
      !equals_case_insensitive(windows_volume.file_system, L"NTFS") ||
      (request.firmware == BcdBootFirmware::uefi
           ? !equals_case_insensitive(system_volume.file_system, L"FAT32")
           : !equals_case_insensitive(system_volume.file_system, L"NTFS"))) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"単独起動修復対象ボリュームの物理対応",
        L"Windows/システム領域が指定ディスクと想定ファイルシステムに一致しません"));
  }

  const auto matches_volume =
      [](const diskmodel::PartitionInfo& partition,
         const BootRepairVolumeLocation& volume) {
        return partition.offset_bytes == volume.starting_offset &&
               partition.size_bytes == volume.extent_length;
      };
  const auto windows_partition = std::find_if(
      disk->partitions.begin(),
      disk->partitions.end(),
      [&](const auto& partition) {
        if (!matches_volume(partition, windows_volume) ||
            partition.style != expected_style) {
          return false;
        }
        return request.firmware == BcdBootFirmware::bios ||
               equals_case_insensitive(partition.type, kBasicDataPartitionType);
      });
  const auto system_partition = std::find_if(
      disk->partitions.begin(),
      disk->partitions.end(),
      [&](const auto& partition) {
        if (!matches_volume(partition, system_volume) ||
            partition.style != expected_style) {
          return false;
        }
        return request.firmware == BcdBootFirmware::uefi
            ? equals_case_insensitive(partition.type, kEfiPartitionType)
            : partition.bootable;
      });
  const auto active_count = std::count_if(
      disk->partitions.begin(),
      disk->partitions.end(),
      [](const auto& partition) { return partition.bootable; });
  if (windows_partition == disk->partitions.end() ||
      system_partition == disk->partitions.end() ||
      (request.firmware == BcdBootFirmware::bios && active_count != 1)) {
    return clonecore::Result<BootRepairTargetSelection>::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"単独起動修復対象パーティションの種別検証",
        L"Windows領域とUEFI ESPまたは一意なBIOS Active領域を対応付けられません"));
  }

  auto identity =
      diskmodel::make_stable_disk_identity(*disk, disk->is_system_disk);
  if (!identity) {
    return clonecore::Result<BootRepairTargetSelection>::failure(
        identity.error());
  }
  return clonecore::Result<BootRepairTargetSelection>::success(
      BootRepairTargetSelection{
          .disk = *disk,
          .identity = identity.take_value(),
          .windows_partition = *windows_partition,
          .system_partition = *system_partition,
      });
}

std::wstring make_boot_repair_confirmation_token(
    const clonecore::StableDiskIdentity& identity,
    const BcdBootFirmware firmware) {
  std::wostringstream stream;
  stream << L"REPAIR BOOT "
         << (firmware == BcdBootFirmware::uefi ? L"UEFI " : L"BIOS ")
         << identity.model << L" ";
  for (const char character : identity.serial_suffix) {
    stream << static_cast<wchar_t>(static_cast<unsigned char>(character));
  }
  stream << L" " << identity.size_bytes;
  return stream.str();
}

clonecore::Status validate_boot_repair_selection(
    const BootRepairTargetSelection& expected,
    const BootRepairTargetSelection& observed,
    const BcdBootFirmware firmware,
    const clonecore::TargetConfirmation& confirmation) {
  const clonecore::Status identity_status = clonecore::validate_stable_identity(
      expected.identity, observed.identity, L"起動修復対象");
  if (!identity_status) {
    return identity_status;
  }
  // Device-health readings (especially temperature) are intentionally not an
  // identity field. Every persistent DiskInfo and complete layout field is
  // nevertheless compared again after confirmation and immediately before
  // the write transaction.
  if (!same_disk_layout_and_identity_fields(expected.disk, observed.disk) ||
      !same_partition(
          expected.windows_partition, observed.windows_partition) ||
      !same_partition(expected.system_partition, observed.system_partition)) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"起動修復対象パーティションの再識別",
        L"確認時と実行直前のWindowsまたはシステム領域が一致しません"));
  }
  if (!confirmation.first_step_acknowledged ||
      confirmation.typed_token !=
          make_boot_repair_confirmation_token(observed.identity, firmware)) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"起動ファイル変更の二段階確認",
        L"確認操作または対象固有の入力確認文字列が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_boot_repair_efi_ownership(
    const BootRepairTargetRequest& request,
    const EfiBootOwnershipEvidence& observed) {
  if (request.update_current_pc_nvram) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"UEFI NVRAM修復方針",
        L"現在PCのNVRAM変更は専用の条件付きtransactionが所有するため、BCD transactionでは実行しません"));
  }
  if (request.firmware != BcdBootFirmware::uefi) {
    if (request.require_efi_ownership_recheck ||
        !request.system_volume_identity_root.empty() ||
        request.third_party_efi_policy !=
            BootRepairThirdPartyEfiPolicy::not_applicable ||
        request.expected_efi_ownership.state !=
            EfiBootOwnershipState::not_applicable ||
        observed.state != EfiBootOwnershipState::not_applicable) {
      return clonecore::Status::failure(repair_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"BIOS起動修復のEFI方針",
          L"BIOS修復要求へUEFI専用のEFI所有権情報が混在しています"));
    }
    return clonecore::success_status();
  }
  if (!request.require_efi_ownership_recheck ||
      request.system_volume_identity_root.empty() ||
      (request.store_policy != BcdBootStorePolicy::rebuild_fresh &&
       !(request.reviewed_multi_windows_batch &&
         request.store_policy == BcdBootStorePolicy::preserve_existing))) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_INVALID_STATE,
        L"UEFI既存ESP安全方針",
        L"既存ESPの読取り専用所有権再検査とBCDBoot /c新規再構築が必須です"));
  }
  if (!equivalent_efi_boot_ownership(
          request.expected_efi_ownership, observed)) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"UEFI ESP所有権再照合",
        L"レビュー時と実行直前のEFI領域または署名状態が一致しません"));
  }
  if (request.third_party_efi_policy ==
      BootRepairThirdPartyEfiPolicy::delete_non_microsoft) {
    return clonecore::Status::failure(repair_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"UEFI第三者EFI削除方針",
        L"第三者EFI削除は専用のimmutable-manifest transactionが所有するため、BCD transactionでは実行しません"));
  }
  switch (observed.state) {
    case EfiBootOwnershipState::microsoft_only_or_empty:
      if (request.third_party_efi_policy !=
              BootRepairThirdPartyEfiPolicy::not_applicable ||
          !efi_boot_ownership_allows_microsoft_rebuild(observed)) {
        return clonecore::Status::failure(repair_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"UEFI第三者EFI保持方針",
            L"第三者EFIがないESPへ保持／削除方針を指定できません"));
      }
      return clonecore::success_status();
    case EfiBootOwnershipState::non_microsoft_or_untrusted_present:
      if (!request.reviewed_multi_windows_batch ||
          request.third_party_efi_policy !=
              BootRepairThirdPartyEfiPolicy::preserve ||
          !efi_boot_ownership_allows_third_party_preserve(observed)) {
        return clonecore::Status::failure(repair_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_NOT_SUPPORTED,
            L"UEFI第三者EFIローダー保護",
            L"レビュー済みの保持方針がないため、第三者EFIを含むESPは変更しません"));
      }
      // BCDBoot receives an explicit /s system root. This reviewed path does
      // not enumerate, rename, or delete any third-party EFI namespace.
      return clonecore::success_status();
    case EfiBootOwnershipState::ambiguous:
    case EfiBootOwnershipState::not_applicable:
    default:
      break;
  }
  return clonecore::Status::failure(repair_error(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"UEFI第三者EFIローダー保護",
      L"EFI内容を安全に分類できないため修復を開始しません"));
}

std::unique_ptr<IStandaloneBootRepairService>
make_windows_standalone_boot_repair_service(
    diskmodel::IDiskInventoryProvider& inventory) {
  return std::make_unique<WindowsStandaloneBootRepairService>(inventory);
}

}  // namespace ytec::bootrepair

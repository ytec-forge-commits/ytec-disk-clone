#include "ytec/bootrepair/automatic_repair_windows.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

clonecore::Error adapter_error(
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
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t left_character, const wchar_t right_character) {
            return std::towlower(left_character) ==
                std::towlower(right_character);
          });
}

bool exact_partition_extent(
    const BootVolumeObservation& volume,
    const diskmodel::PartitionInfo& partition) {
  return volume.location.starting_offset == partition.offset_bytes &&
      volume.location.extent_length == partition.size_bytes;
}

class WindowsBootVolumeEnumerator final
    : public IReadOnlyBootVolumeEnumerator {
 public:
  clonecore::Result<std::vector<BootVolumeObservation>> enumerate_read_only()
      override {
    return enumerate_windows_boot_volumes_read_only();
  }
};

class FilteredBootRepairVolumeObservationProvider final
    : public IBootRepairVolumeObservationProvider {
 public:
  explicit FilteredBootRepairVolumeObservationProvider(
      std::unique_ptr<IReadOnlyBootVolumeEnumerator> enumerator) noexcept
      : enumerator_(std::move(enumerator)) {}

  clonecore::Result<std::vector<BootVolumeObservation>> observe_read_only(
      const diskmodel::DiskInfo& selected_disk) override {
    if (enumerator_ == nullptr) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(adapter_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"自動起動修復のVolume GUID列挙初期化",
          L"読取り専用Volume GUID列挙サービスがありません"));
    }
    auto observations = enumerator_->enumerate_read_only();
    if (!observations) {
      return clonecore::Result<std::vector<BootVolumeObservation>>::failure(
          observations.error());
    }
    return filter_boot_repair_volumes_for_selected_disk(
        selected_disk, observations.take_value());
  }

 private:
  std::unique_ptr<IReadOnlyBootVolumeEnumerator> enumerator_;
};

enum class ExpectedPathKind : std::uint8_t {
  directory,
  regular_file,
};

clonecore::Result<bool> inspect_regular_non_reparse_path(
    const std::wstring& path,
    const ExpectedPathKind expected_kind,
    const bool missing_is_absent,
    const std::wstring_view operation) {
  DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
  if (expected_kind == ExpectedPathKind::directory) {
    flags |= FILE_FLAG_BACKUP_SEMANTICS;
  }
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      0U,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      flags,
      nullptr));
  if (!handle) {
    const DWORD native_code = GetLastError();
    if (missing_is_absent &&
        (native_code == ERROR_FILE_NOT_FOUND ||
         native_code == ERROR_PATH_NOT_FOUND)) {
      return clonecore::Result<bool>::success(false);
    }
    return clonecore::Result<bool>::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed, operation, native_code));
  }

  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          handle.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Result<bool>::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  const bool is_directory =
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  const bool is_reparse =
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
  const bool expected_type = expected_kind == ExpectedPathKind::directory
      ? is_directory
      : !is_directory;
  if (is_reparse || !expected_type) {
    return clonecore::Result<bool>::failure(adapter_error(
        clonecore::ErrorCode::unsupported_layout,
        is_reparse ? ERROR_REPARSE_TAG_INVALID : ERROR_DIRECTORY,
        std::wstring(operation),
        L"Windows候補にreparse pointまたは種類不一致のパスがあります"));
  }
  return clonecore::Result<bool>::success(true);
}

class WindowsOfflineWindowsReadOnlyProbe final
    : public IOfflineWindowsReadOnlyProbe {
 public:
  clonecore::Result<OfflineWindowsRootObservation> inspect_root_read_only(
      const std::wstring& volume_root) override {
    const auto normalized = normalize_offline_windows_volume_root(volume_root);
    if (!normalized) {
      return clonecore::Result<OfflineWindowsRootObservation>::failure(
          normalized.error());
    }
    const std::wstring windows = normalized.value() + L"Windows";
    const auto windows_directory = inspect_regular_non_reparse_path(
        windows,
        ExpectedPathKind::directory,
        true,
        L"オフラインWindowsディレクトリ読取り専用確認");
    if (!windows_directory) {
      return clonecore::Result<OfflineWindowsRootObservation>::failure(
          windows_directory.error());
    }
    if (!windows_directory.value()) {
      return clonecore::Result<OfflineWindowsRootObservation>::success(
          OfflineWindowsRootObservation::absent);
    }

    const std::wstring system32 = windows + L"\\System32";
    const auto system32_directory = inspect_regular_non_reparse_path(
        system32,
        ExpectedPathKind::directory,
        false,
        L"オフラインWindows System32読取り専用確認");
    if (!system32_directory) {
      return clonecore::Result<OfflineWindowsRootObservation>::failure(
          system32_directory.error());
    }
    const auto config_directory = inspect_regular_non_reparse_path(
        system32 + L"\\Config",
        ExpectedPathKind::directory,
        false,
        L"オフラインWindows Config読取り専用確認");
    if (!config_directory) {
      return clonecore::Result<OfflineWindowsRootObservation>::failure(
          config_directory.error());
    }
    const auto kernel = inspect_regular_non_reparse_path(
        system32 + L"\\ntoskrnl.exe",
        ExpectedPathKind::regular_file,
        false,
        L"オフラインWindowsカーネル読取り専用確認");
    if (!kernel) {
      return clonecore::Result<OfflineWindowsRootObservation>::failure(
          kernel.error());
    }
    return clonecore::Result<OfflineWindowsRootObservation>::success(
        OfflineWindowsRootObservation::candidate);
  }

  clonecore::Result<OfflineWindowsVersion> read_version_read_only(
      const std::wstring& volume_root) override {
    const auto normalized = normalize_offline_windows_volume_root(volume_root);
    if (!normalized) {
      return clonecore::Result<OfflineWindowsVersion>::failure(
          normalized.error());
    }
    return read_offline_windows_version_hive(
        normalized.value() + L"Windows\\System32\\Config\\SOFTWARE");
  }

  clonecore::Status verify_supported_read_only(
      const std::wstring& volume_root) override {
    return verify_offline_windows_amd64(volume_root);
  }
};

class OfflineWindowsCandidateValidator final
    : public IOfflineWindowsCandidateValidator {
 public:
  explicit OfflineWindowsCandidateValidator(
      std::unique_ptr<IOfflineWindowsReadOnlyProbe> probe) noexcept
      : probe_(std::move(probe)) {}

  clonecore::Result<OfflineWindowsCandidateValidation>
  inspect_volume_read_only(const std::wstring& volume_root) override {
    if (probe_ == nullptr) {
      return clonecore::Result<
          OfflineWindowsCandidateValidation>::failure(adapter_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"オフラインWindows読取り専用検証初期化",
          L"オフラインWindows検証サービスがありません"));
    }
    return classify_offline_windows_candidate(volume_root, *probe_);
  }

 private:
  std::unique_ptr<IOfflineWindowsReadOnlyProbe> probe_;
};

class AutomaticBootRepairPlanService final
    : public IAutomaticBootRepairPlanService {
 public:
  AutomaticBootRepairPlanService(
      std::unique_ptr<diskmodel::IDiskInventoryProvider> inventory,
      std::unique_ptr<IBootRepairVolumeObservationProvider> volumes,
      std::unique_ptr<IOfflineWindowsCandidateValidator> windows_validator,
      std::unique_ptr<IWinReDiagnosticService> winre_inspector,
      std::unique_ptr<IEfiBootOwnershipInspector>
          efi_ownership_inspector) noexcept
      : inventory_(std::move(inventory)),
        volumes_(std::move(volumes)),
        windows_validator_(std::move(windows_validator)),
        winre_inspector_(std::move(winre_inspector)),
        efi_ownership_inspector_(std::move(efi_ownership_inspector)) {}

  clonecore::Result<AutomaticBootRepairPlan> plan(
      const clonecore::StableDiskIdentity& selected_identity) override {
    return make_planner().plan(selected_identity);
  }

  clonecore::Result<AutomaticBootRepairPlan> plan(
      const diskmodel::DiskInfo& selected_disk) override {
    return make_planner().plan(selected_disk);
  }

 private:
  AutomaticBootRepairPlanner make_planner() {
    return AutomaticBootRepairPlanner(
        *inventory_,
        *volumes_,
        *windows_validator_,
        *winre_inspector_,
        *efi_ownership_inspector_);
  }

  std::unique_ptr<diskmodel::IDiskInventoryProvider> inventory_;
  std::unique_ptr<IBootRepairVolumeObservationProvider> volumes_;
  std::unique_ptr<IOfflineWindowsCandidateValidator> windows_validator_;
  std::unique_ptr<IWinReDiagnosticService> winre_inspector_;
  std::unique_ptr<IEfiBootOwnershipInspector> efi_ownership_inspector_;
};

clonecore::Result<AutomaticBootRepairPlan> missing_service_error() {
  return clonecore::Result<AutomaticBootRepairPlan>::failure(adapter_error(
      clonecore::ErrorCode::internal_error,
      ERROR_INVALID_STATE,
      L"自動起動修復計画サービス初期化",
      L"Windows/WinPE読取り専用サービスを初期化できませんでした"));
}

}  // namespace

clonecore::Result<std::vector<BootVolumeObservation>>
filter_boot_repair_volumes_for_selected_disk(
    const diskmodel::DiskInfo& selected_disk,
    std::vector<BootVolumeObservation> all_volumes) {
  std::vector<BootVolumeObservation> selected;
  selected.reserve(all_volumes.size());
  for (auto& volume : all_volumes) {
    if (volume.location.disk_number != selected_disk.disk_number) {
      continue;
    }
    if (volume.location.extent_length == 0U ||
        volume.location.file_system.empty()) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(adapter_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"自動起動修復の対象ボリューム観測確認",
          L"対象ディスクのボリューム観測が不完全です"));
    }
    const auto normalized =
        normalize_offline_windows_volume_root(volume.volume_name);
    if (!normalized) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(normalized.error());
    }
    volume.volume_name = normalized.value();

    const auto partition = std::find_if(
        selected_disk.partitions.begin(),
        selected_disk.partitions.end(),
        [&](const auto& candidate) {
          return exact_partition_extent(volume, candidate);
        });
    if (partition == selected_disk.partitions.end()) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(adapter_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"自動起動修復の対象ボリューム範囲確認",
          L"対象ディスクの区画と完全一致しないボリューム観測があります"));
    }
    const bool duplicate = std::any_of(
        selected.begin(),
        selected.end(),
        [&](const auto& existing) {
          return equals_case_insensitive(
                     existing.volume_name, volume.volume_name) ||
              (existing.location.starting_offset ==
                   volume.location.starting_offset &&
               existing.location.extent_length ==
                   volume.location.extent_length);
        });
    if (duplicate) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(adapter_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"自動起動修復の対象ボリューム一意性確認",
          L"対象ディスクの同じVolume GUIDまたは区画が複数回報告されました"));
    }
    selected.push_back(std::move(volume));
  }
  return clonecore::Result<std::vector<BootVolumeObservation>>::success(
      std::move(selected));
}

std::unique_ptr<IBootRepairVolumeObservationProvider>
make_filtered_boot_repair_volume_observation_provider(
    std::unique_ptr<IReadOnlyBootVolumeEnumerator> enumerator) {
  if (enumerator == nullptr) {
    return nullptr;
  }
  return std::make_unique<FilteredBootRepairVolumeObservationProvider>(
      std::move(enumerator));
}

std::unique_ptr<IBootRepairVolumeObservationProvider>
make_windows_boot_repair_volume_observation_provider() {
  return make_filtered_boot_repair_volume_observation_provider(
      std::make_unique<WindowsBootVolumeEnumerator>());
}

clonecore::Result<OfflineWindowsCandidateValidation>
classify_offline_windows_candidate(
    const std::wstring& volume_root,
    IOfflineWindowsReadOnlyProbe& probe) {
  const auto normalized = normalize_offline_windows_volume_root(volume_root);
  if (!normalized) {
    return clonecore::Result<OfflineWindowsCandidateValidation>::failure(
        normalized.error());
  }
  auto root = probe.inspect_root_read_only(normalized.value());
  if (!root) {
    return clonecore::Result<OfflineWindowsCandidateValidation>::failure(
        root.error());
  }
  if (root.value() == OfflineWindowsRootObservation::absent) {
    return clonecore::Result<OfflineWindowsCandidateValidation>::success({});
  }
  if (root.value() != OfflineWindowsRootObservation::candidate) {
    return clonecore::Result<OfflineWindowsCandidateValidation>::failure(
        adapter_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"オフラインWindowsルート観測確認",
            L"未知のWindowsルート観測を拒否しました"));
  }

  auto version = probe.read_version_read_only(normalized.value());
  if (!version) {
    return clonecore::Result<OfflineWindowsCandidateValidation>::failure(
        version.error());
  }
  const bool supported_version =
      is_supported_offline_windows_version(version.value());
  const clonecore::Status supported =
      probe.verify_supported_read_only(normalized.value());
  if (supported) {
    if (!supported_version) {
      return clonecore::Result<OfflineWindowsCandidateValidation>::failure(
          adapter_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"オフラインWindows対応状態確認",
              L"版判定とオフラインWindows検証の結果が一致しません"));
    }
    return clonecore::Result<OfflineWindowsCandidateValidation>::success(
        OfflineWindowsCandidateValidation{
            .state = OfflineWindowsCandidateState::present_supported,
            .version = version.take_value(),
        });
  }
  if (!supported_version &&
      supported.error().code == clonecore::ErrorCode::unsupported_layout &&
      supported.error().native_code == ERROR_NOT_SUPPORTED) {
    return clonecore::Result<OfflineWindowsCandidateValidation>::success(
        OfflineWindowsCandidateValidation{
            .state = OfflineWindowsCandidateState::present_unsupported,
            .version = version.take_value(),
        });
  }
  return clonecore::Result<OfflineWindowsCandidateValidation>::failure(
      supported.error());
}

std::unique_ptr<IOfflineWindowsCandidateValidator>
make_offline_windows_candidate_validator(
    std::unique_ptr<IOfflineWindowsReadOnlyProbe> probe) {
  if (probe == nullptr) {
    return nullptr;
  }
  return std::make_unique<OfflineWindowsCandidateValidator>(
      std::move(probe));
}

std::unique_ptr<IOfflineWindowsCandidateValidator>
make_windows_offline_windows_candidate_validator() {
  return make_offline_windows_candidate_validator(
      std::make_unique<WindowsOfflineWindowsReadOnlyProbe>());
}

std::unique_ptr<IAutomaticBootRepairPlanService>
make_automatic_boot_repair_plan_service(
    std::unique_ptr<diskmodel::IDiskInventoryProvider> inventory,
    std::unique_ptr<IBootRepairVolumeObservationProvider> volumes,
    std::unique_ptr<IOfflineWindowsCandidateValidator> windows_validator,
    std::unique_ptr<IWinReDiagnosticService> winre_inspector,
    std::unique_ptr<IEfiBootOwnershipInspector> efi_ownership_inspector) {
  if (inventory == nullptr || volumes == nullptr ||
      windows_validator == nullptr || winre_inspector == nullptr ||
      efi_ownership_inspector == nullptr) {
    return nullptr;
  }
  return std::make_unique<AutomaticBootRepairPlanService>(
      std::move(inventory),
      std::move(volumes),
      std::move(windows_validator),
      std::move(winre_inspector),
      std::move(efi_ownership_inspector));
}

std::unique_ptr<IAutomaticBootRepairPlanService>
make_windows_automatic_boot_repair_plan_service() {
  return make_automatic_boot_repair_plan_service(
      diskmodel::make_windows_disk_inventory_provider(),
      make_windows_boot_repair_volume_observation_provider(),
      make_windows_offline_windows_candidate_validator(),
      make_windows_winre_diagnostic_service(),
      make_windows_efi_boot_ownership_inspector());
}

clonecore::Result<AutomaticBootRepairPlan>
plan_automatic_boot_repair_with_windows_apis(
    const clonecore::StableDiskIdentity& selected_identity) {
  auto service = make_windows_automatic_boot_repair_plan_service();
  return service == nullptr ? missing_service_error()
                            : service->plan(selected_identity);
}

clonecore::Result<AutomaticBootRepairPlan>
plan_automatic_boot_repair_with_windows_apis(
    const diskmodel::DiskInfo& selected_disk) {
  auto service = make_windows_automatic_boot_repair_plan_service();
  return service == nullptr ? missing_service_error()
                            : service->plan(selected_disk);
}

}  // namespace ytec::bootrepair

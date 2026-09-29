#include "ytec/winpeapp/app_runner.h"
#include "boot_finalization.h"

#include "ytec/bootrepair/offline_windows.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>
#include <initguid.h>
#include <objbase.h>
#include <vds.h>
#include <winioctl.h>
#include <winsvc.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kMicrosoftReservedPartitionType =
    L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}";
constexpr std::wstring_view kBasicDataPartitionType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t kMicrosoftReservedPartitionBytes =
    16ULL * kMebibyte;

class ScopedComInitialization final {
 public:
  explicit ScopedComInitialization(const bool uninitialize) noexcept
      : uninitialize_(uninitialize) {}

  ~ScopedComInitialization() noexcept {
    if (uninitialize_) {
      CoUninitialize();
    }
  }

  ScopedComInitialization(const ScopedComInitialization&) = delete;
  ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;

 private:
  bool uninitialize_;
};

class UniqueServiceHandle final {
 public:
  explicit UniqueServiceHandle(SC_HANDLE value = nullptr) noexcept
      : value_(value) {}

  ~UniqueServiceHandle() noexcept {
    if (value_ != nullptr) {
      CloseServiceHandle(value_);
    }
  }

  UniqueServiceHandle(const UniqueServiceHandle&) = delete;
  UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }
  [[nodiscard]] SC_HANDLE get() const noexcept { return value_; }

 private:
  SC_HANDLE value_;
};

template <typename Interface>
class UniqueComInterface final {
 public:
  ~UniqueComInterface() noexcept {
    if (value_ != nullptr) {
      value_->Release();
    }
  }

  UniqueComInterface() noexcept = default;
  UniqueComInterface(const UniqueComInterface&) = delete;
  UniqueComInterface& operator=(const UniqueComInterface&) = delete;

  [[nodiscard]] Interface* get() const noexcept { return value_; }
  [[nodiscard]] Interface** put() noexcept { return &value_; }
  [[nodiscard]] Interface* operator->() const noexcept { return value_; }

 private:
  Interface* value_ = nullptr;
};

clonecore::Error migration_error(
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

bool is_drive_root(const std::wstring_view path) {
  return path.size() == 3U && std::iswalpha(path[0]) != 0 &&
      path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

bool matches_identity(
    const clonecore::StableDiskIdentity& expected,
    const diskmodel::DiskInfo& observed) {
  auto identity = diskmodel::make_stable_disk_identity(
      observed, observed.is_system_disk);
  return identity && clonecore::validate_stable_identity(
      expected, identity.value(), L"移行対象");
}

clonecore::Result<diskmodel::DiskInfo> resolve_stable_disk(
    const clonecore::StableDiskIdentity& expected,
    const diskmodel::InventoryReport& inventory,
    const std::wstring_view role) {
  if (!inventory.issues.empty()) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        std::wstring(role) + L"の全ディスク再列挙",
        L"未解決の列挙診断があるため安定識別を続行できません"));
  }
  std::vector<const diskmodel::DiskInfo*> matches;
  for (const auto& disk : inventory.disks) {
    if (matches_identity(expected, disk)) {
      matches.push_back(&disk);
    }
  }
  if (matches.size() != 1U) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        std::wstring(role) + L"の安定識別",
        L"同じ物理ディスクを一意に再識別できません"));
  }
  return clonecore::Result<diskmodel::DiskInfo>::success(*matches.front());
}

struct MigrationDiskPair final {
  diskmodel::DiskInfo source;
  diskmodel::DiskInfo target;
  clonecore::StableDiskIdentity target_identity;
};

// Once the destructive clone seam is reached, every later exit makes a
// best-effort stable-identity transition back to offline. This also covers the
// narrow failure window after the inner clone returned the target online but
// before it could return a success report. Successful product results
// additionally require an explicit checked transition and a fresh offline
// inventory verification.
class TargetOfflineOnExit final {
 public:
  TargetOfflineOnExit(
      const clonecore::StableDiskIdentity& source,
      const clonecore::StableDiskIdentity& target,
      clonecore::TargetConfirmation confirmation) noexcept
      : source_(source),
        target_(target),
        confirmation_(std::move(confirmation)) {}

  ~TargetOfflineOnExit() noexcept {
    if (armed_) {
      static_cast<void>(
          diskmodel::set_verified_target_offline_with_windows_apis(
              source_, target_, confirmation_, true));
    }
  }

  TargetOfflineOnExit(const TargetOfflineOnExit&) = delete;
  TargetOfflineOnExit& operator=(const TargetOfflineOnExit&) = delete;

  void arm() noexcept { armed_ = true; }
  void disarm() noexcept { armed_ = false; }

 private:
  const clonecore::StableDiskIdentity& source_;
  const clonecore::StableDiskIdentity& target_;
  clonecore::TargetConfirmation confirmation_;
  bool armed_{};
};

clonecore::Result<MigrationDiskPair> observe_migration_pair(
    diskmodel::IDiskInventoryProvider& provider,
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const bool require_gpt_target) {
  auto inventory = provider.enumerate();
  if (!inventory) {
    return clonecore::Result<MigrationDiskPair>::failure(inventory.error());
  }
  auto source = resolve_stable_disk(
      expected_source, inventory.value(), L"MBR移行コピー元");
  if (!source) {
    return clonecore::Result<MigrationDiskPair>::failure(source.error());
  }
  auto target = resolve_stable_disk(
      expected_target, inventory.value(), L"MBR移行コピー先");
  if (!target) {
    return clonecore::Result<MigrationDiskPair>::failure(target.error());
  }
  if (source.value().disk_number == target.value().disk_number ||
      source.value().partition_style != diskmodel::PartitionStyle::mbr ||
      source.value().partitions.empty() || source.value().is_system_disk ||
      target.value().is_system_disk) {
    return clonecore::Result<MigrationDiskPair>::failure(migration_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"MBR移行のコピー元保護",
        L"固定した別ターゲットへのMBR移行で、コピー元を変更しない構成だけを許可します"));
  }
  if (require_gpt_target &&
      target.value().partition_style != diskmodel::PartitionStyle::gpt) {
    return clonecore::Result<MigrationDiskPair>::failure(migration_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"MBR2GPT変換後の形式確認",
        L"Microsoft MBR2GPT成功後にコピー先をGPTとして再確認できません"));
  }
  auto target_identity = diskmodel::make_stable_disk_identity(
      target.value(), target.value().is_system_disk);
  if (!target_identity) {
    return clonecore::Result<MigrationDiskPair>::failure(
        target_identity.error());
  }
  return clonecore::Result<MigrationDiskPair>::success(MigrationDiskPair{
      .source = source.take_value(),
      .target = target.take_value(),
      .target_identity = target_identity.take_value(),
  });
}

clonecore::Status validate_source_before_mbr_clone(
    const diskmodel::DiskInfo& source) {
  if (source.partition_style != diskmodel::PartitionStyle::mbr ||
      source.partitions.empty() || source.partitions.size() > 3U ||
      diskmodel::disk_health_operation_advice(source.health, true) ==
          diskmodel::DiskHealthOperationAdvice::recommend_rescue) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"MBR→GPT書込み前のコピー元構成",
        L"健康な1～3基本プライマリ領域のMBRコピー元だけを変換できます"));
  }
  const diskmodel::PartitionInfo* active = nullptr;
  for (const auto& partition : source.partitions) {
    if (partition.style != diskmodel::PartitionStyle::mbr ||
        (!equals_case_insensitive(partition.type, L"0x07") &&
         !equals_case_insensitive(partition.type, L"0x27"))) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"MBR→GPT書込み前の基本パーティション",
          L"基本NTFSまたはWindows回復領域以外を含むため開始できません"));
    }
    if (!partition.bootable) {
      continue;
    }
    if (active != nullptr ||
        !equals_case_insensitive(partition.type, L"0x07")) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"MBR→GPT書込み前のActive領域",
          L"Active基本NTFSシステム領域を一意に確認できません"));
    }
    active = &partition;
  }
  if (active == nullptr) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"MBR→GPT書込み前のActive領域",
        L"Active基本NTFSシステム領域を一意に確認できません"));
  }
  const std::array<diskmodel::VolumePartitionLocation, 1U> location{{
      diskmodel::VolumePartitionLocation{
          .table_index = active->number,
          .offset_bytes = active->offset_bytes,
      },
  }};
  auto binding = diskmodel::query_windows_volume_bindings_by_offset(
      source, location);
  if (!binding) {
    return clonecore::Status::failure(binding.error());
  }
  if (binding.value().size() != 1U) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"MBR→GPT書込み前のActiveボリューム",
        L"Active領域を一つのローカルVolume GUIDへ対応付けできません"));
  }
  std::array<wchar_t, 64U> file_system{};
  if (GetVolumeInformationW(
          binding.value().front().volume_device_path.c_str(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size())) == FALSE) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"MBR→GPT書込み前のActiveファイルシステム",
        L"Active領域をNTFSとして読取り確認できません"));
  }
  if (!equals_case_insensitive(file_system.data(), L"NTFS")) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"MBR→GPT書込み前のActiveファイルシステム",
        L"Active領域はNTFSである必要があります"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_converted_layout(
    const diskmodel::DiskInfo& target,
    const bool require_microsoft_reserved_partition = true,
    const bool require_online_target = true) {
  if (target.partition_style != diskmodel::PartitionStyle::gpt ||
      target.partitions.empty() || target.is_system_disk ||
      !target.offline.has_value() ||
      target.offline.value() == require_online_target ||
      !target.read_only.has_value() || target.read_only.value() ||
      !target.removable.has_value() || target.removable.value()) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_NOT_READY,
        L"MBR2GPT変換後のディスク状態",
        require_online_target
            ? L"オンライン・書込み可能・固定の非システムGPTコピー先を確認できません"
            : L"オフライン・書込み可能・固定の非システムGPTコピー先を確認できません"));
  }

  std::size_t efi_count = 0U;
  std::size_t reserved_count = 0U;
  std::size_t basic_count = 0U;
  for (const auto& partition : target.partitions) {
    if (partition.style != diskmodel::PartitionStyle::gpt ||
        partition.number == 0U || partition.size_bytes == 0U) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"MBR2GPT変換後のパーティション情報",
          L"GPTとして確定していない区画情報があります"));
    }
    if (equals_case_insensitive(partition.type, kEfiPartitionType)) {
      ++efi_count;
    } else if (equals_case_insensitive(
                   partition.type, kMicrosoftReservedPartitionType)) {
      ++reserved_count;
    } else if (equals_case_insensitive(
                   partition.type, kBasicDataPartitionType)) {
      ++basic_count;
    }
  }
  const bool invalid_reserved_count =
      require_microsoft_reserved_partition
      ? reserved_count != 1U
      : reserved_count > 1U;
  if (efi_count != 1U || invalid_reserved_count || basic_count == 0U) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"MBR2GPT変換後の必須区画",
        require_microsoft_reserved_partition
            ? L"一意なESP、MSR、およびWindows候補の基本データ区画を確認できません"
            : L"一意なESP、Windows候補の基本データ区画、および最大1個のMSRを確認できません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint64_t> select_msr_creation_offset(
    const diskmodel::DiskInfo& target) {
  if (target.partition_style != diskmodel::PartitionStyle::gpt ||
      target.size_bytes <= 2ULL * kMebibyte +
          kMicrosoftReservedPartitionBytes) {
    return clonecore::Result<std::uint64_t>::failure(migration_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"MBR2GPT後MSR空き領域計画",
        L"GPTコピー先の寸法が16MiB MSRの安全な追加に不足しています"));
  }

  std::vector<const diskmodel::PartitionInfo*> partitions;
  partitions.reserve(target.partitions.size());
  for (const auto& partition : target.partitions) {
    if (partition.offset_bytes > target.size_bytes ||
        partition.size_bytes == 0U ||
        partition.size_bytes > target.size_bytes - partition.offset_bytes) {
      return clonecore::Result<std::uint64_t>::failure(migration_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"MBR2GPT後MSR空き領域計画",
          L"GPTパーティション範囲がコピー先容量外です"));
    }
    partitions.push_back(&partition);
  }
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const auto* left, const auto* right) {
        return left->offset_bytes < right->offset_bytes;
      });

  const auto aligned_mib = [](const std::uint64_t value) {
    return value % kMebibyte == 0U
        ? value
        : value + (kMebibyte - value % kMebibyte);
  };
  std::uint64_t cursor = kMebibyte;
  const std::uint64_t usable_end = target.size_bytes - kMebibyte;
  for (const auto* partition : partitions) {
    const std::uint64_t candidate = aligned_mib(cursor);
    if (candidate <= partition->offset_bytes &&
        kMicrosoftReservedPartitionBytes <=
            partition->offset_bytes - candidate) {
      return clonecore::Result<std::uint64_t>::success(candidate);
    }
    const std::uint64_t partition_end =
        partition->offset_bytes + partition->size_bytes;
    if (partition_end > cursor) {
      cursor = partition_end;
    }
  }
  const std::uint64_t candidate = aligned_mib(cursor);
  if (candidate <= usable_end &&
      kMicrosoftReservedPartitionBytes <= usable_end - candidate) {
    return clonecore::Result<std::uint64_t>::success(candidate);
  }
  return clonecore::Result<std::uint64_t>::failure(migration_error(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_DISK_FULL,
      L"MBR2GPT後MSR空き領域計画",
      L"既存区画を移動せず16MiB MSRを追加できる1MiB整列空き領域がありません"));
}

clonecore::Status refresh_disk_partition_cache(
    const diskmodel::DiskInfo& target) {
  const std::wstring expected_path =
      L"\\\\.\\PhysicalDrive" + std::to_wstring(target.disk_number);
  if (!equals_case_insensitive(target.device_path, expected_path)) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"MBR2GPT変換後の区画キャッシュ更新対象",
        L"再識別したコピー先の物理ディスクパスが番号と一致しません"));
  }

  clonecore::UniqueHandle disk(CreateFileW(
      expected_path.c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!disk) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"MBR2GPT変換後の区画キャッシュ更新対象",
        L"書込み権限なしでコピー先の物理ディスクを開けません"));
  }

  DWORD bytes_returned = 0;
  if (DeviceIoControl(
          disk.get(),
          IOCTL_DISK_UPDATE_PROPERTIES,
          nullptr,
          0,
          nullptr,
          0,
          &bytes_returned,
          nullptr) == FALSE) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"MBR2GPT変換後の区画キャッシュ更新",
        L"コピー先のキャッシュ済み区画表を無効化して再列挙できません"));
  }
  return clonecore::success_status();
}

clonecore::Status refresh_virtual_disk_service_cache(
    const diskmodel::DiskInfo& target) {
  const std::wstring expected_path =
      L"\\\\.\\PhysicalDrive" + std::to_wstring(target.disk_number);
  if (!equals_case_insensitive(target.device_path, expected_path)) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"MBR2GPT変換後のVDSキャッシュ更新対象",
        L"再識別したコピー先の物理ディスクパスが番号と一致しません"));
  }

  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool must_uninitialize = initialized == S_OK || initialized == S_FALSE;
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(initialized),
        L"VDS用COM初期化",
        L"Virtual Disk Serviceの安全な再読込みを開始できません"));
  }
  const ScopedComInitialization com_scope(must_uninitialize);

  UniqueComInterface<IVdsServiceLoader> loader;
  HRESULT result = CoCreateInstance(
      CLSID_VdsLoader,
      nullptr,
      CLSCTX_LOCAL_SERVER,
      IID_PPV_ARGS(loader.put()));
  if (result != S_OK || loader.get() == nullptr) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VDSローダー取得",
        L"Microsoft Virtual Disk Serviceローダーを取得できません"));
  }

  UniqueComInterface<IVdsService> service;
  result = loader->LoadService(nullptr, service.put());
  if (result != S_OK || service.get() == nullptr) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VDSサービス読込み",
        L"ローカルVirtual Disk Serviceを読み込めません"));
  }
  result = service->WaitForServiceReady();
  if (result != S_OK) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VDSサービス初期化待機",
        L"Virtual Disk Serviceの初期化完了を確認できません"));
  }
  result = service->Reenumerate();
  if (result != S_OK) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VDSディスク再列挙",
        L"Virtual Disk Serviceへディスク再列挙を要求できません"));
  }
  result = service->Refresh();
  if (result != S_OK) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VDS区画キャッシュ更新",
        L"Virtual Disk Serviceの既存ディスク区画情報を更新できません"));
  }
  return clonecore::success_status();
}

clonecore::Result<DWORD> query_service_state(const SC_HANDLE service) {
  SERVICE_STATUS_PROCESS status{};
  DWORD bytes_needed = 0U;
  if (QueryServiceStatusEx(
          service,
          SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&status),
          sizeof(status),
          &bytes_needed) == FALSE) {
    return clonecore::Result<DWORD>::failure(migration_error(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"VDSサービス状態取得",
        L"Virtual Disk Serviceの現在状態を取得できません"));
  }
  return clonecore::Result<DWORD>::success(status.dwCurrentState);
}

clonecore::Status wait_for_service_state(
    const SC_HANDLE service,
    const DWORD desired_state,
    const std::wstring_view operation) {
  constexpr DWORD kPollIntervalMilliseconds = 100U;
  constexpr std::size_t kMaximumPollCount = 300U;
  for (std::size_t index = 0U; index < kMaximumPollCount; ++index) {
    auto current = query_service_state(service);
    if (!current) {
      return clonecore::Status::failure(current.error());
    }
    if (current.value() == desired_state) {
      return clonecore::success_status();
    }
    Sleep(kPollIntervalMilliseconds);
  }
  return clonecore::Status::failure(migration_error(
      clonecore::ErrorCode::query_failed,
      ERROR_TIMEOUT,
      std::wstring(operation),
      L"Virtual Disk Serviceの状態遷移が30秒以内に完了しません"));
}

clonecore::Status restart_virtual_disk_service(
    const diskmodel::DiskInfo& target) {
  const std::wstring expected_path =
      L"\\\\.\\PhysicalDrive" + std::to_wstring(target.disk_number);
  if (!equals_case_insensitive(target.device_path, expected_path)) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"VDSサービス再初期化対象",
        L"再識別したコピー先の物理ディスクパスが番号と一致しません"));
  }

  UniqueServiceHandle manager(OpenSCManagerW(
      nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::access_denied,
        GetLastError(),
        L"サービス制御マネージャー接続",
        L"VDSサービスを安全に再初期化する権限を取得できません"));
  }
  UniqueServiceHandle service(OpenServiceW(
      manager.get(),
      L"vds",
      SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP));
  if (!service) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::access_denied,
        GetLastError(),
        L"VDSサービス制御ハンドル取得",
        L"Microsoft Virtual Disk Serviceを開けません"));
  }

  auto current = query_service_state(service.get());
  if (!current) {
    return clonecore::Status::failure(current.error());
  }
  if (current.value() == SERVICE_START_PENDING) {
    const auto started = wait_for_service_state(
        service.get(), SERVICE_RUNNING, L"VDS開始完了待機");
    if (!started) {
      return started;
    }
    current = clonecore::Result<DWORD>::success(SERVICE_RUNNING);
  } else if (current.value() == SERVICE_STOP_PENDING) {
    const auto stopped = wait_for_service_state(
        service.get(), SERVICE_STOPPED, L"VDS停止完了待機");
    if (!stopped) {
      return stopped;
    }
    current = clonecore::Result<DWORD>::success(SERVICE_STOPPED);
  }

  if (current.value() != SERVICE_STOPPED) {
    if (current.value() != SERVICE_RUNNING &&
        current.value() != SERVICE_PAUSED) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::query_failed,
          ERROR_SERVICE_CANNOT_ACCEPT_CTRL,
          L"VDSサービス停止前状態検証",
          L"Virtual Disk Serviceが安全に停止できる状態ではありません"));
    }
    SERVICE_STATUS stopped_status{};
    if (ControlService(
            service.get(), SERVICE_CONTROL_STOP, &stopped_status) == FALSE) {
      const DWORD stop_error = GetLastError();
      if (stop_error != ERROR_SERVICE_NOT_ACTIVE) {
        return clonecore::Status::failure(migration_error(
            clonecore::ErrorCode::query_failed,
            stop_error,
            L"VDSサービス停止要求",
            L"古い区画情報を破棄するためのVDS停止要求に失敗しました"));
      }
    }
    const auto stopped = wait_for_service_state(
        service.get(), SERVICE_STOPPED, L"VDS停止完了待機");
    if (!stopped) {
      return stopped;
    }
  }

  if (StartServiceW(service.get(), 0U, nullptr) == FALSE) {
    const DWORD start_error = GetLastError();
    if (start_error != ERROR_SERVICE_ALREADY_RUNNING) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::query_failed,
          start_error,
          L"VDSサービス再開要求",
          L"最新のGPT区画情報でVDSを再開できません"));
    }
  }
  return wait_for_service_state(
      service.get(), SERVICE_RUNNING, L"VDS再開完了待機");
}

clonecore::Result<std::wstring> current_system_directory() {
  std::vector<wchar_t> buffer(32U * 1024U, L'\0');
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"MBR2GPT用System32パス取得",
            length == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(buffer.data(), length));
}

class WindowsMbr2GptTargetObserver final
    : public bootrepair::IMbr2GptTargetObserver {
 public:
  explicit WindowsMbr2GptTargetObserver(
      diskmodel::IDiskInventoryProvider& provider)
      : provider_(provider) {}

  clonecore::Result<clonecore::StableDiskIdentity> observe_target(
      const std::uint32_t candidate_disk_number) override {
    auto inventory = provider_.enumerate();
    if (!inventory) {
      return clonecore::Result<clonecore::StableDiskIdentity>::failure(
          inventory.error());
    }
    if (!inventory.value().issues.empty()) {
      return clonecore::Result<clonecore::StableDiskIdentity>::failure(
          migration_error(
              clonecore::ErrorCode::query_failed,
              ERROR_INVALID_DATA,
              L"MBR2GPT直前の全ディスク列挙",
              L"未解決の列挙診断があるため変換先を確定できません"));
    }
    const auto disk = std::find_if(
        inventory.value().disks.begin(),
        inventory.value().disks.end(),
        [&](const diskmodel::DiskInfo& item) {
          return item.disk_number == candidate_disk_number;
        });
    if (disk == inventory.value().disks.end()) {
      return clonecore::Result<clonecore::StableDiskIdentity>::failure(
          migration_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"MBR2GPT変換先のディスク番号再確認",
              L"指定番号の物理ディスクを再列挙結果から確認できません"));
    }
    return diskmodel::make_stable_disk_identity(*disk, disk->is_system_disk);
  }

 private:
  diskmodel::IDiskInventoryProvider& provider_;
};

std::optional<std::wstring> select_unused_drive_root(
    const std::vector<bootrepair::BootVolumeObservation>& volumes) {
  std::array<bool, 26> used{};
  const DWORD drive_mask = GetLogicalDrives();
  if (drive_mask == 0U) {
    return std::nullopt;
  }
  for (std::size_t index = 0U; index < used.size(); ++index) {
    used[index] = (drive_mask & (1UL << index)) != 0U;
  }
  for (const auto& volume : volumes) {
    for (const auto& mount_point : volume.mount_points) {
      if (is_drive_root(mount_point)) {
        const wchar_t upper = static_cast<wchar_t>(
            std::towupper(mount_point[0]));
        used[static_cast<std::size_t>(upper - L'A')] = true;
      }
    }
  }
  used[static_cast<std::size_t>(L'X' - L'A')] = true;
  for (wchar_t letter = L'Y'; letter >= L'D'; --letter) {
    if (!used[static_cast<std::size_t>(letter - L'A')]) {
      return std::wstring(1U, letter) + L":\\";
    }
  }
  return std::nullopt;
}

clonecore::Result<bool> has_offline_windows_kernel(
    const std::wstring& root) {
  const std::wstring path = root + L"Windows\\System32\\ntoskrnl.exe";
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<bool>::success(false);
    }
    return clonecore::Result<bool>::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"変換後Windowsカーネル候補確認",
        native_code));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Result<bool>::failure(migration_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_REPARSE_TAG_INVALID,
        L"変換後Windowsカーネル候補検証",
        L"通常ファイルではないWindowsカーネル候補を拒否しました"));
  }
  return clonecore::Result<bool>::success(true);
}

bool valid_direct_mount_plan(
    const std::wstring_view root,
    const std::uint32_t disk_number,
    const std::uint32_t partition_number,
    const bootrepair::BootRepairVolumeLocation& expected) {
  return is_drive_root(root) && root[2] == L'\\' &&
      partition_number != 0U && expected.disk_number == disk_number &&
      expected.extent_length != 0U;
}

clonecore::Status verify_direct_partition_handle(
    const HANDLE handle,
    const std::uint32_t partition_number,
    const bootrepair::BootRepairVolumeLocation& expected,
    const std::wstring_view operation) {
  STORAGE_DEVICE_NUMBER device{};
  DWORD device_returned = 0U;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_GET_DEVICE_NUMBER,
          nullptr,
          0U,
          &device,
          static_cast<DWORD>(sizeof(device)),
          &device_returned,
          nullptr)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        std::wstring(operation) + L"の物理デバイス番号",
        GetLastError()));
  }
  if (device_returned < sizeof(device) ||
      device.DeviceType != FILE_DEVICE_DISK ||
      device.DeviceNumber != expected.disk_number ||
      device.PartitionNumber != partition_number) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"直接パーティションの物理ディスク番号または区画番号が計画と一致しません"));
  }

  PARTITION_INFORMATION_EX partition{};
  DWORD returned = 0U;
  if (!DeviceIoControl(
          handle,
          IOCTL_DISK_GET_PARTITION_INFO_EX,
          nullptr,
          0U,
          &partition,
          static_cast<DWORD>(sizeof(partition)),
          &returned,
          nullptr)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        std::wstring(operation),
        GetLastError()));
  }
  if (returned < sizeof(partition) ||
      partition.PartitionNumber != partition_number ||
      partition.StartingOffset.QuadPart < 0 ||
      partition.PartitionLength.QuadPart <= 0 ||
      static_cast<std::uint64_t>(
          partition.StartingOffset.QuadPart) != expected.starting_offset ||
      static_cast<std::uint64_t>(
          partition.PartitionLength.QuadPart) != expected.extent_length) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"直接パーティションの番号、開始位置、または長さが計画と一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status run_verified_diskpart_script(
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const std::string_view command,
    const std::wstring_view operation) {
  if (trusted_system_directory.size() < 3U ||
      trusted_system_directory[1] != L':' ||
      trusted_system_directory[2] != L'\\' ||
      command.empty() || operation.empty()) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"DiskPart固定スクリプト引数",
        L"WinPEシステムパス、固定コマンド、または操作名が不正です"));
  }

  const std::wstring diskpart_path =
      trusted_system_directory + L"\\diskpart.exe";
  const auto trust = trust_verifier.verify_microsoft_signed(diskpart_path);
  if (!trust) {
    return trust;
  }

  const std::wstring temp_directory =
      trusted_system_directory.substr(0U, 3U) + L"Windows\\Temp\\";
  const DWORD temp_attributes =
      GetFileAttributesW(temp_directory.c_str());
  if (temp_attributes == INVALID_FILE_ATTRIBUTES ||
      (temp_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (temp_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::invalid_data,
        temp_attributes == INVALID_FILE_ATTRIBUTES
            ? GetLastError()
            : ERROR_REPARSE_TAG_INVALID,
        L"DiskPart一時スクリプト保存先",
        L"WinPE RAMディスク上の通常ディレクトリを確認できません"));
  }

  std::wstring script_path;
  clonecore::UniqueHandle script_file;
  for (std::uint32_t attempt = 0U; attempt < 16U; ++attempt) {
    script_path = temp_directory +
        L"YTEC-Tsumugi-Drive-diskpart-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" +
        std::to_wstring(attempt) + L".txt";
    script_file.reset(CreateFileW(
        script_path.c_str(),
        GENERIC_WRITE,
        0U,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (script_file) {
      break;
    }
    if (GetLastError() != ERROR_FILE_EXISTS &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"DiskPart一時スクリプト作成",
          GetLastError()));
    }
  }
  if (!script_file) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::io_failed,
        ERROR_ALREADY_EXISTS,
        L"DiskPart一時スクリプト作成",
        L"衝突しない一時ファイルを作成できません"));
  }

  DWORD written = 0U;
  const bool write_ok = WriteFile(
      script_file.get(),
      command.data(),
      static_cast<DWORD>(command.size()),
      &written,
      nullptr) != FALSE;
  const DWORD write_error = write_ok ? ERROR_SUCCESS : GetLastError();
  const bool flush_ok = write_ok && FlushFileBuffers(script_file.get()) != FALSE;
  const DWORD flush_error = flush_ok ? ERROR_SUCCESS : GetLastError();
  script_file.reset();
  if (!write_ok || written != command.size() || !flush_ok) {
    static_cast<void>(DeleteFileW(script_path.c_str()));
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"DiskPart一時スクリプト書込み",
        !write_ok ? write_error
                  : (!flush_ok ? flush_error : ERROR_WRITE_FAULT)));
  }

  auto process = process_runner.run(
      diskpart_path,
      {L"/s", script_path},
      trusted_system_directory);
  const bool deleted = DeleteFileW(script_path.c_str()) != FALSE;
  const DWORD delete_error = deleted ? ERROR_SUCCESS : GetLastError();
  if (!deleted) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"DiskPart一時スクリプト削除",
        delete_error));
  }
  if (!process) {
    return clonecore::Status::failure(process.error());
  }
  if (process.value().exit_code != 0U) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::io_failed,
        process.value().exit_code,
        std::wstring(operation),
        L"Microsoft DiskPartが失敗終了しました"));
  }
  return clonecore::success_status();
}

clonecore::Status run_diskpart_partition_letter_command(
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const std::uint32_t disk_number,
    const std::uint32_t partition_number,
    const wchar_t drive_letter,
    const bool assign) {
  if (drive_letter < L'D' || drive_letter > L'Z' ||
      drive_letter == L'X' || partition_number == 0U) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"DiskPart一時ドライブ文字コマンド引数",
        L"ドライブ文字または物理区画指定が不正です"));
  }
  const std::string command =
      "select disk " + std::to_string(disk_number) + "\r\n" +
      "select partition " + std::to_string(partition_number) + "\r\n" +
      (assign ? "assign letter=" : "remove letter=") +
      std::string(1U, static_cast<char>(drive_letter)) + "\r\nexit\r\n";
  return run_verified_diskpart_script(
      trusted_system_directory,
      trust_verifier,
      process_runner,
      command,
      assign ? L"DiskPart一時ドライブ文字割当"
             : L"DiskPart一時ドライブ文字解除");
}

clonecore::Status run_diskpart_rescan_command(
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner) {
  return run_verified_diskpart_script(
      trusted_system_directory,
      trust_verifier,
      process_runner,
      "rescan\r\nexit\r\n",
      L"DiskPart変換後ディスク再スキャン");
}

clonecore::Status run_diskpart_create_msr_command(
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const std::uint32_t disk_number,
    const std::uint64_t offset_bytes) {
  constexpr std::uint64_t kKilobyte = 1024ULL;
  if (offset_bytes == 0U || offset_bytes % kMebibyte != 0U) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"DiskPart MSR作成コマンド引数",
        L"MSR開始位置は正の1MiB境界で指定する必要があります"));
  }
  const std::string command =
      "select disk " + std::to_string(disk_number) + "\r\n" +
      "create partition msr size=16 offset=" +
      std::to_string(offset_bytes / kKilobyte) + "\r\nexit\r\n";
  return run_verified_diskpart_script(
      trusted_system_directory,
      trust_verifier,
      process_runner,
      command,
      L"DiskPart 16MiB MSR作成");
}

class DirectPartitionMount final {
 public:
  ~DirectPartitionMount() noexcept {
    if (mounted_) {
      static_cast<void>(remove_definition());
    }
  }

  DirectPartitionMount(const DirectPartitionMount&) = delete;
  DirectPartitionMount& operator=(const DirectPartitionMount&) = delete;
  DirectPartitionMount(DirectPartitionMount&& other) noexcept
      : root_(std::move(other.root_)),
        drive_name_(std::move(other.drive_name_)),
        file_system_(std::move(other.file_system_)),
        trusted_system_directory_(
            std::move(other.trusted_system_directory_)),
        trust_verifier_(other.trust_verifier_),
        process_runner_(other.process_runner_),
        disk_number_(other.disk_number_),
        partition_number_(other.partition_number_),
        expected_(std::move(other.expected_)),
        mounted_(other.mounted_) {
    other.mounted_ = false;
    other.trust_verifier_ = nullptr;
    other.process_runner_ = nullptr;
  }
  DirectPartitionMount& operator=(DirectPartitionMount&&) = delete;

  const std::wstring& root() const noexcept { return root_; }
  const std::wstring& file_system() const noexcept { return file_system_; }

  static clonecore::Result<DirectPartitionMount> acquire(
      const std::wstring& root,
      const std::uint32_t disk_number,
      const std::uint32_t partition_number,
      const bootrepair::BootRepairVolumeLocation& expected,
      const std::wstring_view expected_file_system,
      const std::wstring& trusted_system_directory,
      bootrepair::IExecutableTrustVerifier& trust_verifier,
      bootrepair::IProcessRunner& process_runner) {
    if (!valid_direct_mount_plan(
            root, disk_number, partition_number, expected) ||
        (!expected_file_system.empty() &&
         expected_file_system != L"NTFS" &&
         expected_file_system != L"FAT32")) {
      return clonecore::Result<DirectPartitionMount>::failure(
          migration_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"MBR2GPTコピー先の直接一時割当引数",
              L"ドライブ文字または物理パーティション計画が不正です"));
    }
    const wchar_t letter = static_cast<wchar_t>(std::towupper(root[0]));
    const DWORD mask = GetLogicalDrives();
    const DWORD bit = 1UL << static_cast<DWORD>(letter - L'A');
    if (mask == 0U || (mask & bit) != 0U) {
      return clonecore::Result<DirectPartitionMount>::failure(
          migration_error(
              clonecore::ErrorCode::identity_mismatch,
              mask == 0U ? GetLastError() : ERROR_ALREADY_ASSIGNED,
              L"MBR2GPTコピー先の直接一時割当直前確認",
              L"計画したドライブ文字が使用中または状態不明です"));
    }

    const std::wstring drive_name =
        std::wstring(1U, letter) + L":";
    std::array<wchar_t, 8> existing{};
    if (QueryDosDeviceW(
            drive_name.c_str(), existing.data(),
            static_cast<DWORD>(existing.size())) != 0U ||
        GetLastError() != ERROR_FILE_NOT_FOUND) {
      return clonecore::Result<DirectPartitionMount>::failure(
          migration_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_ALREADY_ASSIGNED,
              L"MBR2GPTコピー先のDOSデバイス重複確認",
              L"未使用のはずのドライブ文字にDOSデバイス定義があります"));
    }
    DirectPartitionMount mounted(
        root,
        drive_name,
        trusted_system_directory,
        trust_verifier,
        process_runner,
        disk_number,
        partition_number,
        expected);
    const auto assignment = run_diskpart_partition_letter_command(
        trusted_system_directory,
        trust_verifier,
        process_runner,
        disk_number,
        partition_number,
        letter,
        true);
    if (!assignment) {
      return clonecore::Result<DirectPartitionMount>::failure(
          assignment.error());
    }

    const std::wstring drive_open = L"\\\\.\\" + drive_name;
    clonecore::UniqueHandle mounted_partition(CreateFileW(
        drive_open.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!mounted_partition) {
      return clonecore::Result<DirectPartitionMount>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"MBR2GPTコピー先の直接割当後読取り",
              GetLastError()));
    }
    const auto after = verify_direct_partition_handle(
        mounted_partition.get(), partition_number, expected,
        L"MBR2GPTコピー先の直接割当後再識別");
    if (!after) {
      return clonecore::Result<DirectPartitionMount>::failure(after.error());
    }
    std::array<wchar_t, MAX_PATH> file_system{};
    const bool file_system_available = GetVolumeInformationW(
            root.c_str(), nullptr, 0U, nullptr, nullptr, nullptr,
            file_system.data(),
            static_cast<DWORD>(file_system.size())) != FALSE;
    if (!file_system_available ||
        (!expected_file_system.empty() &&
         !equals_case_insensitive(
             file_system.data(), expected_file_system))) {
      const DWORD native = file_system_available
          ? ERROR_INVALID_DATA
          : GetLastError();
      return clonecore::Result<DirectPartitionMount>::failure(
          migration_error(
              clonecore::ErrorCode::verification_failed,
              native == ERROR_SUCCESS ? ERROR_INVALID_DATA : native,
              L"MBR2GPTコピー先の直接割当ファイルシステム確認",
              L"割当後のコピー先パーティションを期待ファイルシステムとして確認できません"));
    }
    mounted.file_system_ = file_system.data();
    return clonecore::Result<DirectPartitionMount>::success(
        std::move(mounted));
  }

  clonecore::Status release() {
    if (!mounted_) {
      return clonecore::success_status();
    }
    const auto status = remove_definition();
    if (status) {
      mounted_ = false;
    }
    return status;
  }

 private:
  DirectPartitionMount(
      std::wstring root,
      std::wstring drive_name,
      std::wstring trusted_system_directory,
      bootrepair::IExecutableTrustVerifier& trust_verifier,
      bootrepair::IProcessRunner& process_runner,
      const std::uint32_t disk_number,
      const std::uint32_t partition_number,
      bootrepair::BootRepairVolumeLocation expected)
      : root_(std::move(root)),
        drive_name_(std::move(drive_name)),
        trusted_system_directory_(std::move(trusted_system_directory)),
        trust_verifier_(&trust_verifier),
        process_runner_(&process_runner),
        disk_number_(disk_number),
        partition_number_(partition_number),
        expected_(std::move(expected)),
        mounted_(true) {}

  clonecore::Status remove_definition() noexcept {
    try {
      if (trust_verifier_ == nullptr || process_runner_ == nullptr) {
        return clonecore::Status::failure(migration_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_STATE,
            L"DiskPart一時ドライブ文字解除状態",
            L"解除に必要な実行サービスを確認できません"));
      }
      const DWORD bit = 1UL << static_cast<DWORD>(root_[0] - L'A');
      const DWORD before_mask = GetLogicalDrives();
      if (before_mask == 0U) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"DiskPart一時ドライブ文字解除前確認",
            GetLastError()));
      }
      std::array<wchar_t, 1024> before_target{};
      const bool definition_present = QueryDosDeviceW(
          drive_name_.c_str(), before_target.data(),
          static_cast<DWORD>(before_target.size())) != 0U;
      const DWORD definition_error =
          definition_present ? ERROR_SUCCESS : GetLastError();
      if ((before_mask & bit) == 0U && !definition_present &&
          definition_error == ERROR_FILE_NOT_FOUND) {
        return clonecore::success_status();
      }
      if (!definition_present) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"DiskPart一時ドライブ文字解除前DOSデバイス確認",
            definition_error));
      }

      const std::wstring drive_open = L"\\\\.\\" + drive_name_;
      clonecore::UniqueHandle mounted_partition(CreateFileW(
          drive_open.c_str(),
          GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL,
          nullptr));
      if (!mounted_partition) {
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"DiskPart一時ドライブ文字解除前読取り",
            GetLastError()));
      }
      const auto identity = verify_direct_partition_handle(
          mounted_partition.get(), partition_number_, expected_,
          L"DiskPart一時ドライブ文字解除前物理再識別");
      if (!identity) {
        return identity;
      }
      mounted_partition.reset();

      const auto removal = run_diskpart_partition_letter_command(
          trusted_system_directory_,
          *trust_verifier_,
          *process_runner_,
          disk_number_,
          partition_number_,
          root_[0],
          false);
      if (!removal) {
        return removal;
      }
      for (std::uint32_t attempt = 0U; attempt < 20U; ++attempt) {
        const DWORD after_mask = GetLogicalDrives();
        if (after_mask == 0U) {
          return clonecore::Status::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::query_failed,
                  L"DiskPart一時ドライブ文字解除後確認",
                  GetLastError()));
        }
        std::array<wchar_t, 8> remaining{};
        const bool still_defined = QueryDosDeviceW(
            drive_name_.c_str(), remaining.data(),
            static_cast<DWORD>(remaining.size())) != 0U;
        const DWORD remaining_error =
            still_defined ? ERROR_SUCCESS : GetLastError();
        if ((after_mask & bit) == 0U && !still_defined &&
            remaining_error == ERROR_FILE_NOT_FOUND) {
          return clonecore::success_status();
        }
        Sleep(50U);
      }
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_ALREADY_ASSIGNED,
          L"DiskPart一時ドライブ文字解除後確認",
          L"解除後もドライブ文字の割当が残っています"));
    } catch (...) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"MBR2GPTコピー先の直接一時割当解除例外",
          L"解除処理で予期しない例外を捕捉しました"));
    }
  }

  std::wstring root_;
  std::wstring drive_name_;
  std::wstring file_system_;
  std::wstring trusted_system_directory_;
  bootrepair::IExecutableTrustVerifier* trust_verifier_{};
  bootrepair::IProcessRunner* process_runner_{};
  std::uint32_t disk_number_{};
  std::uint32_t partition_number_{};
  bootrepair::BootRepairVolumeLocation expected_;
  bool mounted_{};
};

struct LocatedWindowsVolume final {
  std::wstring root;
  bootrepair::BootRepairVolumeLocation location;
  std::optional<DirectPartitionMount> direct_mount;
};

clonecore::Status release_windows_mount(LocatedWindowsVolume& located) {
  if (located.direct_mount.has_value()) {
    return located.direct_mount->release();
  }
  return clonecore::success_status();
}

clonecore::Result<std::unique_ptr<LocatedWindowsVolume>>
locate_target_windows_volume(
    const diskmodel::DiskInfo& target,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const std::optional<std::uint64_t> expected_starting_offset =
        std::nullopt) {
  const auto is_windows_candidate = [&](
                                        const diskmodel::PartitionInfo&
                                            partition) {
    return (target.partition_style == diskmodel::PartitionStyle::gpt &&
            partition.style == diskmodel::PartitionStyle::gpt &&
            equals_case_insensitive(
                partition.type, kBasicDataPartitionType)) ||
        (target.partition_style == diskmodel::PartitionStyle::mbr &&
         partition.style == diskmodel::PartitionStyle::mbr &&
         equals_case_insensitive(partition.type, L"0x07"));
  };

  // Volume GUID interfaces can be stale while Windows reconciles a newly
  // cloned or converted layout.  Select only physical partitions from the
  // refreshed target inventory, verify the partition handle before and after
  // a temporary DOS-device mapping, and inspect that exact mapping.
  std::vector<const diskmodel::PartitionInfo*> physical_candidates;
  for (const auto& partition : target.partitions) {
    if (is_windows_candidate(partition) &&
        (!expected_starting_offset.has_value() ||
         partition.offset_bytes == expected_starting_offset.value())) {
      physical_candidates.push_back(&partition);
    }
  }
  if (expected_starting_offset.has_value() &&
      physical_candidates.size() != 1U) {
    return clonecore::Result<
        std::unique_ptr<LocatedWindowsVolume>>::failure(migration_error(
        clonecore::ErrorCode::identity_mismatch,
        physical_candidates.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"変換後Windows領域の物理対応",
        L"変換前Windows領域と同じ開始位置の基本データ区画を一意に確認できません"));
  }

  std::unique_ptr<LocatedWindowsVolume> located;
  for (const auto* partition : physical_candidates) {
    const auto root = select_unused_drive_root({});
    if (!root.has_value()) {
      return clonecore::Result<
          std::unique_ptr<LocatedWindowsVolume>>::failure(migration_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NO_MORE_ITEMS,
          L"Windows領域の直接一時ドライブ文字",
          L"安全に使用できる空きドライブ文字がありません"));
    }
    const bootrepair::BootRepairVolumeLocation physical_location{
        .disk_number = target.disk_number,
        .starting_offset = partition->offset_bytes,
        .extent_length = partition->size_bytes,
        .file_system = {},
    };
    auto mounted = DirectPartitionMount::acquire(
        root.value(), target.disk_number, partition->number,
        physical_location, {}, trusted_system_directory,
        trust_verifier, process_runner);
    if (!mounted) {
      const DWORD native = mounted.error().native_code;
      if (!expected_starting_offset.has_value() &&
          (native == ERROR_UNRECOGNIZED_VOLUME ||
           native == ERROR_NOT_READY)) {
        continue;
      }
      return clonecore::Result<
          std::unique_ptr<LocatedWindowsVolume>>::failure(mounted.error());
    }
    if (!equals_case_insensitive(mounted.value().file_system(), L"NTFS")) {
      const auto cleanup = mounted.value().release();
      if (!cleanup) {
        return clonecore::Result<
            std::unique_ptr<LocatedWindowsVolume>>::failure(cleanup.error());
      }
      if (expected_starting_offset.has_value()) {
        return clonecore::Result<
            std::unique_ptr<LocatedWindowsVolume>>::failure(migration_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"変換後Windows領域のファイルシステム確認",
            L"物理対応した区画をNTFSとして確認できません"));
      }
      continue;
    }

    auto candidate = std::make_unique<LocatedWindowsVolume>();
    candidate->root = mounted.value().root();
    candidate->location = physical_location;
    candidate->location.file_system = mounted.value().file_system();
    candidate->direct_mount.emplace(mounted.take_value());

    const auto kernel = has_offline_windows_kernel(candidate->root);
    if (!kernel) {
      const auto cleanup = release_windows_mount(*candidate);
      return clonecore::Result<
          std::unique_ptr<LocatedWindowsVolume>>::failure(
          cleanup ? kernel.error() : cleanup.error());
    }
    if (!kernel.value()) {
      const auto cleanup = release_windows_mount(*candidate);
      if (!cleanup) {
        return clonecore::Result<
            std::unique_ptr<LocatedWindowsVolume>>::failure(cleanup.error());
      }
      if (expected_starting_offset.has_value()) {
        return clonecore::Result<
            std::unique_ptr<LocatedWindowsVolume>>::failure(migration_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_FILE_NOT_FOUND,
            L"変換後Windows領域のカーネル確認",
            L"物理対応した区画にWindowsカーネルがありません"));
      }
      continue;
    }
    const auto windows_status =
        bootrepair::verify_offline_windows_amd64(candidate->root);
    if (!windows_status) {
      const auto cleanup = release_windows_mount(*candidate);
      return clonecore::Result<
          std::unique_ptr<LocatedWindowsVolume>>::failure(
          cleanup ? windows_status.error() : cleanup.error());
    }
    if (located != nullptr) {
      const auto candidate_cleanup = release_windows_mount(*candidate);
      const auto existing_cleanup = release_windows_mount(*located);
      if (!candidate_cleanup) {
        return clonecore::Result<
            std::unique_ptr<LocatedWindowsVolume>>::failure(
            candidate_cleanup.error());
      }
      if (!existing_cleanup) {
        return clonecore::Result<
            std::unique_ptr<LocatedWindowsVolume>>::failure(
            existing_cleanup.error());
      }
      return clonecore::Result<
          std::unique_ptr<LocatedWindowsVolume>>::failure(migration_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"変換後Windows領域の一意選択",
          L"対応Windows 10/11 x64を複数検出したため自動選択しません"));
    }
    located = std::move(candidate);
  }
  if (located == nullptr) {
    return clonecore::Result<std::unique_ptr<LocatedWindowsVolume>>::failure(
        migration_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_NOT_FOUND,
            expected_starting_offset.has_value()
                ? L"変換後Windows領域の確認"
                : L"変換前Windows領域の確認",
            L"コピー先の物理区画から対応Windows 10/11 x64を一意に確認できません"));
  }
  return clonecore::Result<std::unique_ptr<LocatedWindowsVolume>>::success(
      std::move(located));
}

void report_stage(
    const clonecore::DiskOperationCallbacks& callbacks,
    clonecore::DiskOperationProgress progress,
    const clonecore::DiskOperationStage stage,
    const bool cancellation_allowed) {
  progress.stage = stage;
  progress.partition_index.reset();
  progress.cancellation_allowed = cancellation_allowed;
  clonecore::report_disk_operation_progress(callbacks, progress);
}

clonecore::Status rebuild_target_bios_boot_before_conversion(
    const diskmodel::DiskInfo& target,
    LocatedWindowsVolume& windows,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    diskmodel::IDiskInventoryProvider& inventory,
    const clonecore::DiskOperationCallbacks& callbacks,
    const clonecore::DiskOperationProgress& last_progress) {
  const auto active_count = std::count_if(
      target.partitions.begin(),
      target.partitions.end(),
      [](const diskmodel::PartitionInfo& partition) {
        return partition.style == diskmodel::PartitionStyle::mbr &&
            partition.bootable;
      });
  const auto active_partition = std::find_if(
      target.partitions.begin(),
      target.partitions.end(),
      [](const diskmodel::PartitionInfo& partition) {
        return partition.style == diskmodel::PartitionStyle::mbr &&
            partition.bootable &&
            equals_case_insensitive(partition.type, L"0x07");
      });
  if (target.partition_style != diskmodel::PartitionStyle::mbr ||
      active_count != 1 || active_partition == target.partitions.end()) {
    return clonecore::Status::failure(migration_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"MBR2GPT前のBIOS起動領域選択",
        L"一意なActive NTFS MBRシステム領域を確認できません"));
  }

  const bool windows_is_system_partition =
      active_partition->offset_bytes == windows.location.starting_offset &&
      active_partition->size_bytes == windows.location.extent_length;
  std::optional<DirectPartitionMount> separate_system_mount;
  std::wstring system_root = windows.root;
  if (!windows_is_system_partition) {
    const auto root = select_unused_drive_root({});
    if (!root.has_value()) {
      return clonecore::Status::failure(migration_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NO_MORE_ITEMS,
          L"MBR2GPT前のBIOS起動領域一時ドライブ文字",
          L"Active MBRシステム領域へ安全に使用できる空きドライブ文字がありません"));
    }
    auto mounted = DirectPartitionMount::acquire(
        root.value(),
        target.disk_number,
        active_partition->number,
        bootrepair::BootRepairVolumeLocation{
            .disk_number = target.disk_number,
            .starting_offset = active_partition->offset_bytes,
            .extent_length = active_partition->size_bytes,
            .file_system = L"NTFS",
        },
        L"NTFS",
        trusted_system_directory,
        trust_verifier,
        process_runner);
    if (!mounted) {
      return clonecore::Status::failure(mounted.error());
    }
    separate_system_mount.emplace(mounted.take_value());
    system_root = separate_system_mount->root();
  }

  const auto release_system_mount = [&]() -> clonecore::Status {
    return separate_system_mount.has_value()
        ? separate_system_mount->release()
        : clonecore::success_status();
  };
  const auto fail_after_cleanup = [&](
                                      const clonecore::Error& error) {
    const auto cleanup = release_system_mount();
    return clonecore::Status::failure(
        cleanup ? error : cleanup.error());
  };

  auto boot_service =
      bootrepair::make_windows_standalone_boot_repair_service(inventory);
  const bootrepair::BootRepairTargetRequest boot_request{
      .disk_number = target.disk_number,
      .windows_root = windows.root,
      .system_root = system_root,
      .firmware = bootrepair::BcdBootFirmware::bios,
      .store_policy =
          bootrepair::BcdBootStorePolicy::rebuild_fresh,
      .auto_mount_system_partition = false,
  };
  report_stage(
      callbacks,
      last_progress,
      clonecore::DiskOperationStage::rebuilding_boot,
      false);
  auto selection = boot_service->inspect(boot_request);
  if (!selection) {
    return fail_after_cleanup(selection.error());
  }
  const clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = true,
      .typed_token = bootrepair::make_boot_repair_confirmation_token(
          selection.value().identity,
          bootrepair::BcdBootFirmware::bios),
  };
  auto repaired = boot_service->execute(
      bootrepair::StandaloneBootRepairExecutionRequest{
          .target = boot_request,
          .expected = selection.value(),
          .confirmation = confirmation,
      });
  if (!repaired) {
    return fail_after_cleanup(repaired.error());
  }
  if (!repaired.value().bcdboot.microsoft_signature_verified ||
      repaired.value().bcdboot.exit_code != 0U ||
      !repaired.value().boot_store_verified ||
      repaired.value().system_partition_temporarily_mounted ||
      repaired.value().temporary_mount_released) {
    return fail_after_cleanup(migration_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"MBR2GPT前のBIOS起動再構築",
        L"Microsoft署名、BIOS BCDBoot、BCDストア、または明示一時割当境界を確認できません"));
  }
  return release_system_mount();
}

class MigrationProgressProcessRunner final
    : public bootrepair::IProcessRunner {
 public:
  MigrationProgressProcessRunner(
      bootrepair::IProcessRunner& inner,
      const clonecore::DiskOperationCallbacks& callbacks,
      const clonecore::DiskOperationProgress& last_progress)
      : inner_(inner),
        callbacks_(callbacks),
        last_progress_(last_progress) {}

  clonecore::Result<bootrepair::ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    const bool converting =
        !arguments.empty() && arguments.front() == L"/convert";
    report_stage(
        callbacks_,
        last_progress_,
        converting
            ? clonecore::DiskOperationStage::converting_partition_style
            : clonecore::DiskOperationStage::validating_conversion,
        false);
    return inner_.run(executable_path, arguments, working_directory);
  }

 private:
  bootrepair::IProcessRunner& inner_;
  const clonecore::DiskOperationCallbacks& callbacks_;
  clonecore::DiskOperationProgress last_progress_;
};

class WindowsMbr2GptDirectExecutionService final
    : public IMbr2GptDirectExecutionService {
 public:
  clonecore::Result<Mbr2GptDirectExecutionReport> execute(
      const Mbr2GptDirectExecutionRequest& request) override {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    const auto system_directory = current_system_directory();
    if (!system_directory) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          system_directory.error());
    }
    auto trust = bootrepair::make_windows_authenticode_verifier();
    for (const std::wstring_view tool :
         {L"mbr2gpt.exe", L"bcdboot.exe", L"diskpart.exe"}) {
      const auto trusted = trust->verify_microsoft_signed(
          system_directory.value() + L"\\" + std::wstring(tool));
      if (!trusted) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            trusted.error());
      }
    }
    auto process = bootrepair::make_windows_process_runner();
    TargetOfflineOnExit target_offline_guard(
        request.expected_source,
        request.expected_target,
        request.confirmation);
    const auto protected_source =
        diskmodel::set_verified_source_read_only_with_windows_apis(
            request.expected_source, true);
    if (!protected_source) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          protected_source.error());
    }
    auto source_check = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        false);
    if (!source_check) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          source_check.error());
    }
    const auto source_preflight =
        validate_source_before_mbr_clone(source_check.value().source);
    if (!source_preflight) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          source_preflight.error());
    }
    clonecore::DiskOperationProgress last_progress;
    clonecore::DiskOperationCallbacks clone_callbacks{
        .progress =
            [&](const clonecore::DiskOperationProgress& progress) {
              last_progress = progress;
              if (progress.stage !=
                  clonecore::DiskOperationStage::completed) {
                clonecore::report_disk_operation_progress(
                    request.callbacks, progress);
              }
            },
        .cancellation_requested = request.callbacks.cancellation_requested,
    };
    auto clone_service = make_windows_clone_execution_service();
    // Final read-only inventory snapshot immediately before the destructive
    // seam. Stable identity alone is insufficient: bind both complete,
    // order-normalized partition layouts to the reviewed plan.
    auto before_clone = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        false);
    if (!before_clone) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          before_clone.error());
    }
    const auto reviewed_layouts = validate_mbr2gpt_reviewed_layouts(
        request.expected_source_layout,
        request.expected_target_layout,
        before_clone.value().source,
        before_clone.value().target);
    if (!reviewed_layouts) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          reviewed_layouts.error());
    }
    target_offline_guard.arm();
    auto cloned = clone_service->execute(CloneExecutionRequest{
        .expected_source = request.expected_source,
        .expected_target = request.expected_target,
        .confirmation = request.confirmation,
        .authorization = {},
        .callbacks = std::move(clone_callbacks),
        .leave_target_offline = false,
    });
    if (!cloned) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          cloned.error());
    }
    if (cloned.value().partition_style != ClonePartitionStyle::mbr ||
        cloned.value().copied_data_bytes == 0U ||
        cloned.value().copied_partition_count == 0U ||
        !cloned.value().read_back_verified ||
        !cloned.value().partition_table_committed ||
        !cloned.value().target_returned_online) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          migration_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"MBRからGPT移行のMBRクローン結果",
              L"MBRコピー、読戻し、確定、またはオンライン復帰が完了していません"));
    }
    if (clonecore::disk_operation_cancellation_requested(
            request.callbacks)) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          migration_error(
              clonecore::ErrorCode::cancelled,
              ERROR_CANCELLED,
              L"MBR2GPT変換開始前の取消",
              L"MBRクローンは完了しましたが、GPT変換を開始せず停止しました"));
    }

    auto before_conversion = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        false);
    if (!before_conversion) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          before_conversion.error());
    }
    if (before_conversion.value().target.partition_style !=
            diskmodel::PartitionStyle::mbr ||
        before_conversion.value().target.partitions.empty()) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          migration_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"MBR2GPT変換直前のコピー先形式",
              L"MBRクローン済みコピー先をMBRとして再確認できません"));
    }

    // MBR2GPT supports only Windows 10/11 x64 in this product. Verify the
    // cloned target before invoking the Microsoft converter, then remove any
    // temporary drive letter so conversion starts from a clean mount state.
    auto pre_conversion_windows = locate_target_windows_volume(
        before_conversion.value().target,
        system_directory.value(),
        *trust,
        *process);
    if (!pre_conversion_windows) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          pre_conversion_windows.error());
    }
    const auto pre_conversion_windows_start =
        pre_conversion_windows.value()->location.starting_offset;
    const auto pre_conversion_bios_boot =
        rebuild_target_bios_boot_before_conversion(
            before_conversion.value().target,
            *pre_conversion_windows.value(),
            system_directory.value(),
            *trust,
            *process,
            *inventory,
            request.callbacks,
            last_progress);
    const auto pre_conversion_release =
        release_windows_mount(*pre_conversion_windows.value());
    if (!pre_conversion_bios_boot) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          pre_conversion_release
              ? pre_conversion_bios_boot.error()
              : pre_conversion_release.error());
    }
    if (!pre_conversion_release) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          pre_conversion_release.error());
    }

    MigrationProgressProcessRunner progress_process(
        *process, request.callbacks, last_progress);
    WindowsMbr2GptTargetObserver observer(*inventory);
    auto converted = bootrepair::execute_mbr2gpt_conversion(
        bootrepair::Mbr2GptConversionRequest{
            .candidate_disk_number =
                before_conversion.value().target.disk_number,
            .expected_target = before_conversion.value().target_identity,
            .confirmation = request.confirmation,
        },
        system_directory.value(),
        observer,
        *trust,
        progress_process);
    if (!converted) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          converted.error());
    }
    const auto refreshed = refresh_disk_partition_cache(
        before_conversion.value().target);
    if (!refreshed) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          refreshed.error());
    }
    auto after_conversion = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        true);
    if (!after_conversion) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          after_conversion.error());
    }
    const auto layout_status = validate_converted_layout(
        after_conversion.value().target, false);
    if (!layout_status) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          layout_status.error());
    }

    const auto existing_msr = std::find_if(
        after_conversion.value().target.partitions.begin(),
        after_conversion.value().target.partitions.end(),
        [](const diskmodel::PartitionInfo& partition) {
          return equals_case_insensitive(
              partition.type, kMicrosoftReservedPartitionType);
        });
    std::optional<std::uint64_t> created_msr_offset;
    if (existing_msr ==
        after_conversion.value().target.partitions.end()) {
      auto msr_offset = select_msr_creation_offset(
          after_conversion.value().target);
      if (!msr_offset) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            msr_offset.error());
      }
      report_stage(
          request.callbacks,
          last_progress,
          clonecore::DiskOperationStage::staging_partition_table,
          false);
      // MBR2GPT can leave VDS at the old MBR four-partition view even after
      // the disk driver sees GPT. Refresh that cache using Microsoft's
      // documented Reenumerate-then-Refresh sequence before DiskPart starts.
      const auto vds_refreshed = refresh_virtual_disk_service_cache(
          after_conversion.value().target);
      if (!vds_refreshed) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            vds_refreshed.error());
      }
      // A same-session MBR2GPT conversion can leave the WinPE basic provider
      // at the four-primary MBR limit even after Refresh succeeds. Release and
      // restart only VDS so its new provider instance reads the verified GPT.
      const auto vds_restarted = restart_virtual_disk_service(
          after_conversion.value().target);
      if (!vds_restarted) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            vds_restarted.error());
      }
      // Keep a separate signed DiskPart rescan process so the MSR command is
      // issued only by a newly connected client after the VDS refresh.
      const auto diskpart_refreshed = run_diskpart_rescan_command(
          system_directory.value(), *trust, *process);
      if (!diskpart_refreshed) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            diskpart_refreshed.error());
      }
      const auto created = run_diskpart_create_msr_command(
          system_directory.value(),
          *trust,
          *process,
          after_conversion.value().target.disk_number,
          msr_offset.value());
      if (!created) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            created.error());
      }
      created_msr_offset = msr_offset.value();
    }

    auto normalized_conversion = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        true);
    if (!normalized_conversion) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          normalized_conversion.error());
    }
    const auto normalized_layout = validate_converted_layout(
        normalized_conversion.value().target);
    if (!normalized_layout) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          normalized_layout.error());
    }
    if (created_msr_offset.has_value()) {
      const auto created_msr = std::find_if(
          normalized_conversion.value().target.partitions.begin(),
          normalized_conversion.value().target.partitions.end(),
          [&](const diskmodel::PartitionInfo& partition) {
            return equals_case_insensitive(
                       partition.type,
                       kMicrosoftReservedPartitionType) &&
                partition.offset_bytes == created_msr_offset.value() &&
                partition.size_bytes ==
                    kMicrosoftReservedPartitionBytes;
          });
      if (created_msr ==
          normalized_conversion.value().target.partitions.end()) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            migration_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"MBR2GPT後MSR読戻し検証",
                L"固定位置へ作成した16MiB MSRを再列挙結果で確認できません"));
      }
    }

    auto windows = locate_target_windows_volume(
        normalized_conversion.value().target,
        system_directory.value(),
        *trust,
        *process,
        pre_conversion_windows_start);
    if (!windows) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          windows.error());
    }
    const bool windows_temporarily_mounted =
        windows.value()->direct_mount.has_value();
    const auto efi_partition = std::find_if(
        normalized_conversion.value().target.partitions.begin(),
        normalized_conversion.value().target.partitions.end(),
        [](const diskmodel::PartitionInfo& partition) {
          return partition.style == diskmodel::PartitionStyle::gpt &&
              equals_case_insensitive(partition.type, kEfiPartitionType);
        });
    if (efi_partition ==
        normalized_conversion.value().target.partitions.end()) {
      const auto cleanup = release_windows_mount(*windows.value());
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          cleanup ? migration_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_NOT_FOUND,
              L"MBR2GPT後ESPの直接一時割当",
              L"変換後ターゲットからESPを一意に取得できません")
                  : cleanup.error());
    }
    const auto system_root = select_unused_drive_root({});
    if (!system_root.has_value()) {
      const auto cleanup = release_windows_mount(*windows.value());
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          cleanup ? migration_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NO_MORE_ITEMS,
              L"MBR2GPT後ESPの直接一時ドライブ文字",
              L"ESPへ安全に使用できる空きドライブ文字がありません")
                  : cleanup.error());
    }
    auto mounted_system = DirectPartitionMount::acquire(
        system_root.value(),
        normalized_conversion.value().target.disk_number,
        efi_partition->number,
        bootrepair::BootRepairVolumeLocation{
            .disk_number = normalized_conversion.value().target.disk_number,
            .starting_offset = efi_partition->offset_bytes,
            .extent_length = efi_partition->size_bytes,
            .file_system = L"FAT32",
        },
        L"FAT32",
        system_directory.value(),
        *trust,
        *process);
    if (!mounted_system) {
      const auto cleanup = release_windows_mount(*windows.value());
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          cleanup ? mounted_system.error() : cleanup.error());
    }
    std::optional<DirectPartitionMount> system_mount;
    system_mount.emplace(mounted_system.take_value());
    const auto release_all_mounts = [&]() -> clonecore::Status {
      const auto system_release = system_mount->release();
      const auto windows_release =
          release_windows_mount(*windows.value());
      return system_release ? windows_release : system_release;
    };
    auto boot_service =
        bootrepair::make_windows_standalone_boot_repair_service(*inventory);
    const bootrepair::BootRepairTargetRequest boot_request{
        .disk_number = normalized_conversion.value().target.disk_number,
        .windows_root = windows.value()->root,
        .system_root = system_mount->root(),
        .firmware = bootrepair::BcdBootFirmware::uefi,
        .store_policy =
            bootrepair::BcdBootStorePolicy::rebuild_fresh,
        .auto_mount_system_partition = false,
    };
    report_stage(
        request.callbacks,
        last_progress,
        clonecore::DiskOperationStage::rebuilding_boot,
        false);
    auto boot_selection = boot_service->inspect(boot_request);
    if (!boot_selection) {
      const auto cleanup = release_all_mounts();
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          cleanup ? boot_selection.error() : cleanup.error());
    }
    const clonecore::TargetConfirmation boot_confirmation{
        .first_step_acknowledged = true,
        .typed_token = bootrepair::make_boot_repair_confirmation_token(
            boot_selection.value().identity,
            bootrepair::BcdBootFirmware::uefi),
    };
    auto boot = boot_service->execute(
        bootrepair::StandaloneBootRepairExecutionRequest{
            .target = boot_request,
            .expected = boot_selection.value(),
            .confirmation = boot_confirmation,
        });
    if (!boot) {
      const auto cleanup = release_all_mounts();
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          cleanup ? boot.error() : cleanup.error());
    }
    if (!boot.value().bcdboot.microsoft_signature_verified ||
        boot.value().bcdboot.exit_code != 0U ||
        !boot.value().boot_store_verified ||
        (boot.value().system_partition_temporarily_mounted &&
       !boot.value().temporary_mount_released)) {
      const auto cleanup = release_all_mounts();
      if (!cleanup) {
        return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
            cleanup.error());
      }
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          migration_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"MBR2GPT後のUEFI起動再構築",
              L"Microsoft署名、BCDBoot、BCDストア、またはESP一時割当解除を確認できません"));
    }
    const auto mount_release = release_all_mounts();
    if (!mount_release) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          mount_release.error());
    }

    report_stage(
        request.callbacks,
        last_progress,
        clonecore::DiskOperationStage::verifying_final,
        false);
    auto final_pair = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        true);
    if (!final_pair) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          final_pair.error());
    }
    const auto final_layout = validate_converted_layout(
        final_pair.value().target);
    if (!final_layout) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          final_layout.error());
    }
    const auto final_offline =
        diskmodel::set_verified_target_offline_with_windows_apis(
            request.expected_source,
            request.expected_target,
            request.confirmation,
            true);
    if (!final_offline) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          final_offline.error());
    }
    auto offline_pair = observe_migration_pair(
        *inventory,
        request.expected_source,
        request.expected_target,
        true);
    if (!offline_pair) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          offline_pair.error());
    }
    if (!offline_pair.value().source.read_only.has_value() ||
        !offline_pair.value().source.read_only.value()) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          migration_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_WRITE_PROTECT,
              L"MBR→GPT完了後のコピー元保護",
              L"コピー元をread-onlyのまま再確認できません"));
    }
    const auto offline_layout = validate_converted_layout(
        offline_pair.value().target, true, false);
    if (!offline_layout) {
      return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
          offline_layout.error());
    }
    target_offline_guard.disarm();
    report_stage(
        request.callbacks,
        last_progress,
        clonecore::DiskOperationStage::completed,
        false);

    return clonecore::Result<Mbr2GptDirectExecutionReport>::success(
        Mbr2GptDirectExecutionReport{
            .clone = cloned.take_value(),
            .conversion = converted.take_value(),
            .boot_repair = boot.take_value(),
            .source_reidentified_unchanged = true,
            .source_left_read_only = true,
            .target_reidentified_as_gpt = true,
            .efi_system_partition_verified = true,
            .microsoft_reserved_partition_verified = true,
            .offline_windows_verified = true,
            .windows_partition_temporarily_mounted =
                windows_temporarily_mounted,
            .temporary_windows_mount_released = true,
            .final_layout_verified = true,
            .final_target_left_offline = true,
        });
  }
};

}  // namespace

clonecore::Result<ProductBootFinalizationReport>
finalize_product_target_boot(
    diskmodel::IDiskInventoryProvider& inventory,
    const clonecore::StableDiskIdentity& expected_target,
    const diskmodel::PartitionStyle expected_style,
    const std::optional<std::uint64_t> expected_windows_partition_offset,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (expected_style != diskmodel::PartitionStyle::gpt &&
      expected_style != diskmodel::PartitionStyle::mbr) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        migration_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"製品起動最終化のパーティション形式",
            L"GPTまたはMBRの復元・クローン先だけを起動最終化できます"));
  }
  auto observed_inventory = inventory.enumerate();
  if (!observed_inventory) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        observed_inventory.error());
  }
  auto target = resolve_stable_disk(
      expected_target, observed_inventory.value(), L"起動最終化対象");
  if (!target) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        target.error());
  }
  if (target.value().partition_style != expected_style ||
      target.value().partitions.empty() || target.value().is_system_disk ||
      !target.value().offline.has_value() || target.value().offline.value() ||
      !target.value().read_only.has_value() || target.value().read_only.value() ||
      !target.value().removable.has_value() || target.value().removable.value()) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        migration_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_NOT_READY,
            L"製品起動最終化の対象状態",
            L"形式一致・オンライン・書込み可能・固定の非システム対象を確認できません"));
  }

  const auto system_directory = current_system_directory();
  if (!system_directory) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        system_directory.error());
  }
  auto trust = bootrepair::make_windows_authenticode_verifier();
  auto process = bootrepair::make_windows_process_runner();
  auto windows = locate_target_windows_volume(
      target.value(),
      system_directory.value(),
      *trust,
      *process,
      expected_windows_partition_offset);
  if (!windows) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        windows.error());
  }
  const bool windows_temporarily_mounted =
      windows.value()->direct_mount.has_value();

  const auto firmware = expected_style == diskmodel::PartitionStyle::gpt
      ? bootrepair::BcdBootFirmware::uefi
      : bootrepair::BcdBootFirmware::bios;
  std::vector<const diskmodel::PartitionInfo*> system_candidates;
  for (const auto& partition : target.value().partitions) {
    const bool matches = firmware == bootrepair::BcdBootFirmware::uefi
        ? partition.style == diskmodel::PartitionStyle::gpt &&
            equals_case_insensitive(partition.type, kEfiPartitionType)
        : partition.style == diskmodel::PartitionStyle::mbr &&
            partition.bootable &&
            equals_case_insensitive(partition.type, L"0x07");
    if (matches) {
      system_candidates.push_back(&partition);
    }
  }
  if (system_candidates.size() != 1U) {
    const auto cleanup = release_windows_mount(*windows.value());
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        cleanup ? migration_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"製品起動最終化のシステム領域選択",
            L"一意なESPまたはBIOS Active NTFS領域を確認できません")
                : cleanup.error());
  }
  const auto& system_partition = *system_candidates.front();
  const bool windows_is_system_partition =
      system_partition.offset_bytes == windows.value()->location.starting_offset &&
      system_partition.size_bytes == windows.value()->location.extent_length;
  std::optional<DirectPartitionMount> system_mount;
  std::wstring system_root = windows.value()->root;
  if (!windows_is_system_partition) {
    const auto root = select_unused_drive_root({});
    if (!root.has_value()) {
      const auto cleanup = release_windows_mount(*windows.value());
      return clonecore::Result<ProductBootFinalizationReport>::failure(
          cleanup ? migration_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NO_MORE_ITEMS,
              L"製品起動最終化のシステム領域一時割当",
              L"システム領域へ安全に使用できる空きドライブ文字がありません")
                  : cleanup.error());
    }
    auto mounted = DirectPartitionMount::acquire(
        root.value(),
        target.value().disk_number,
        system_partition.number,
        bootrepair::BootRepairVolumeLocation{
            .disk_number = target.value().disk_number,
            .starting_offset = system_partition.offset_bytes,
            .extent_length = system_partition.size_bytes,
            .file_system = firmware == bootrepair::BcdBootFirmware::uefi
                ? L"FAT32"
                : L"NTFS",
        },
        firmware == bootrepair::BcdBootFirmware::uefi ? L"FAT32" : L"NTFS",
        system_directory.value(),
        *trust,
        *process);
    if (!mounted) {
      const auto cleanup = release_windows_mount(*windows.value());
      return clonecore::Result<ProductBootFinalizationReport>::failure(
          cleanup ? mounted.error() : cleanup.error());
    }
    system_mount.emplace(mounted.take_value());
    system_root = system_mount->root();
  }
  const bool system_temporarily_mounted = system_mount.has_value();
  const auto release_all = [&]() -> clonecore::Status {
    const auto system_release = system_mount.has_value()
        ? system_mount->release()
        : clonecore::success_status();
    const auto windows_release = release_windows_mount(*windows.value());
    return system_release ? windows_release : system_release;
  };
  const auto fail_after_cleanup = [&](const clonecore::Error& error) {
    const auto cleanup = release_all();
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        cleanup ? error : cleanup.error());
  };

  clonecore::DiskOperationProgress progress;
  report_stage(
      callbacks,
      progress,
      clonecore::DiskOperationStage::rebuilding_boot,
      false);
  auto boot_service =
      bootrepair::make_windows_standalone_boot_repair_service(inventory);
  const bootrepair::BootRepairTargetRequest boot_request{
      .disk_number = target.value().disk_number,
      .windows_root = windows.value()->root,
      .system_root = system_root,
      .firmware = firmware,
      .store_policy =
          bootrepair::BcdBootStorePolicy::rebuild_fresh,
      .auto_mount_system_partition = false,
  };
  auto selection = boot_service->inspect(boot_request);
  if (!selection) {
    return fail_after_cleanup(selection.error());
  }
  const clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = true,
      .typed_token = bootrepair::make_boot_repair_confirmation_token(
          selection.value().identity, firmware),
  };
  auto repaired = boot_service->execute(
      bootrepair::StandaloneBootRepairExecutionRequest{
          .target = boot_request,
          .expected = selection.value(),
          .confirmation = confirmation,
      });
  if (!repaired) {
    return fail_after_cleanup(repaired.error());
  }
  if (!repaired.value().bcdboot.microsoft_signature_verified ||
      repaired.value().bcdboot.exit_code != 0U ||
      !repaired.value().boot_store_verified ||
      repaired.value().system_partition_temporarily_mounted ||
      repaired.value().temporary_mount_released) {
    return fail_after_cleanup(migration_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"製品復元・クローン後の起動再構築",
        L"Microsoft署名、BCDBoot、BCDストア、または明示一時割当境界を確認できません"));
  }
  const auto cleanup = release_all();
  if (!cleanup) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        cleanup.error());
  }

  report_stage(
      callbacks,
      progress,
      clonecore::DiskOperationStage::verifying_final,
      false);
  auto final_inventory = inventory.enumerate();
  if (!final_inventory) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        final_inventory.error());
  }
  auto final_target = resolve_stable_disk(
      expected_target, final_inventory.value(), L"起動最終化完了対象");
  if (!final_target || final_target.value().partition_style != expected_style ||
      final_target.value().is_system_disk ||
      !final_target.value().offline.has_value() ||
      final_target.value().offline.value()) {
    return clonecore::Result<ProductBootFinalizationReport>::failure(
        final_target ? migration_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"製品起動最終化の最終再識別",
            L"起動再構築後の対象形式またはオンライン状態を確認できません")
                     : final_target.error());
  }
  report_stage(
      callbacks,
      progress,
      clonecore::DiskOperationStage::completed,
      false);
  return clonecore::Result<ProductBootFinalizationReport>::success(
      ProductBootFinalizationReport{
          .boot_repair = repaired.take_value(),
          .windows_partition_temporarily_mounted =
              windows_temporarily_mounted,
          .system_partition_temporarily_mounted =
              system_temporarily_mounted,
          .temporary_mounts_released = true,
          .final_target_verified = true,
      });
}

std::unique_ptr<IMbr2GptDirectExecutionService>
make_windows_mbr2gpt_direct_execution_service() {
  return std::make_unique<WindowsMbr2GptDirectExecutionService>();
}

}  // namespace ytec::winpeapp

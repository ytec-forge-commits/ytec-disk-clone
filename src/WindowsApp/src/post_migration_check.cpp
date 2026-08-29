#include "ytec/windowsapp/post_migration_check.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/clonecore/error.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>
#include <Wbemidl.h>
#include <oleauto.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kMbrNtfsPartitionType = L"0x07";

clonecore::Error check_error(
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

bool valid_volume_name(const std::wstring_view value) {
  constexpr std::wstring_view kPrefix = L"\\\\?\\Volume{";
  if (value.size() != 49U || !value.starts_with(kPrefix) ||
      !value.ends_with(L"}\\")) {
    return false;
  }
  for (std::size_t index = 11U; index < 47U; ++index) {
    const std::size_t guid_index = index - 11U;
    const bool hyphen = guid_index == 8U || guid_index == 13U ||
        guid_index == 18U || guid_index == 23U;
    if (hyphen) {
      if (value[index] != L'-') {
        return false;
      }
      continue;
    }
    const wchar_t character = value[index];
    const bool hexadecimal =
        (character >= L'0' && character <= L'9') ||
        (character >= L'A' && character <= L'F') ||
        (character >= L'a' && character <= L'f');
    if (!hexadecimal) {
      return false;
    }
  }
  return true;
}

bool valid_file_system(const std::wstring_view value) {
  return equals_case_insensitive(value, L"NTFS") ||
      equals_case_insensitive(value, L"FAT32") ||
      equals_case_insensitive(value, L"exFAT");
}

bool valid_partition_number(
    const diskmodel::DiskInfo& disk,
    const std::uint32_t number) {
  return number != 0U &&
      std::count_if(
          disk.partitions.begin(),
          disk.partitions.end(),
          [number](const diskmodel::PartitionInfo& partition) {
            return partition.number == number;
          }) == 1;
}

bool valid_evidence(const PostMigrationEvidence& evidence) {
  if (evidence.summary.empty() || evidence.detail.empty()) {
    return false;
  }
  if (evidence.state == PostMigrationEvidenceState::verified) {
    return !evidence.error.has_value();
  }
  return true;
}

PostMigrationEvidence verified_evidence(
    std::wstring summary,
    std::wstring detail) {
  return PostMigrationEvidence{
      .state = PostMigrationEvidenceState::verified,
      .summary = std::move(summary),
      .detail = std::move(detail),
  };
}

PostMigrationEvidence attention_evidence(
    std::wstring summary,
    std::wstring detail) {
  return PostMigrationEvidence{
      .state = PostMigrationEvidenceState::attention,
      .summary = std::move(summary),
      .detail = std::move(detail),
  };
}

PostMigrationEvidence unavailable_evidence(
    std::wstring summary,
    std::wstring detail,
    const clonecore::Error& error) {
  return PostMigrationEvidence{
      .state = PostMigrationEvidenceState::unavailable,
      .summary = std::move(summary),
      .detail = std::move(detail),
      .error = error,
  };
}

const bootrepair::BootVolumeObservation* find_volume_for_partition(
    const std::vector<bootrepair::BootVolumeObservation>& volumes,
    const std::uint32_t disk_number,
    const diskmodel::PartitionInfo& partition) {
  const auto found = std::find_if(
      volumes.begin(),
      volumes.end(),
      [&](const bootrepair::BootVolumeObservation& volume) {
        return volume.location.disk_number == disk_number &&
            volume.location.starting_offset == partition.offset_bytes &&
            volume.location.extent_length == partition.size_bytes;
      });
  return found == volumes.end() ? nullptr : &*found;
}

const diskmodel::PartitionInfo* find_partition_for_volume(
    const diskmodel::DiskInfo& disk,
    const bootrepair::BootVolumeObservation& volume) {
  const auto found = std::find_if(
      disk.partitions.begin(),
      disk.partitions.end(),
      [&](const diskmodel::PartitionInfo& partition) {
        return volume.location.disk_number == disk.disk_number &&
            volume.location.starting_offset == partition.offset_bytes &&
            volume.location.extent_length == partition.size_bytes;
      });
  return found == disk.partitions.end() ? nullptr : &*found;
}

std::wstring append_volume_path(
    const std::wstring& volume_name,
    const std::wstring_view relative_path) {
  return volume_name + std::wstring(relative_path);
}

std::wstring get_windows_directory() {
  std::vector<wchar_t> buffer(32768U, L'\0');
  const UINT length = GetWindowsDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return {};
  }
  return std::wstring(buffer.data(), length);
}

clonecore::Result<std::wstring> windows_volume_name(
    const std::wstring& windows_directory) {
  std::array<wchar_t, MAX_PATH> volume_path{};
  if (GetVolumePathNameW(
          windows_directory.c_str(),
          volume_path.data(),
          static_cast<DWORD>(volume_path.size())) == FALSE) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"現在Windowsのボリュームルート取得",
            GetLastError()));
  }
  std::array<wchar_t, 64U> volume_name{};
  if (GetVolumeNameForVolumeMountPointW(
          volume_path.data(),
          volume_name.data(),
          static_cast<DWORD>(volume_name.size())) == FALSE) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"現在WindowsのVolume GUID取得",
            GetLastError()));
  }
  std::wstring result(volume_name.data());
  if (!valid_volume_name(result)) {
    return clonecore::Result<std::wstring>::failure(check_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"現在WindowsのVolume GUID形式",
        L"現在Windowsの正規Volume GUIDを確認できません"));
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

std::wstring current_drive_letter(const std::wstring& windows_directory) {
  if (windows_directory.size() < 3U ||
      !std::iswalpha(windows_directory[0]) ||
      windows_directory[1] != L':' || windows_directory[2] != L'\\') {
    return {};
  }
  std::wstring result;
  result.push_back(static_cast<wchar_t>(
      std::towupper(windows_directory[0])));
  result.push_back(L':');
  return result;
}

class ComApartment final {
 public:
  ComApartment() noexcept {
    result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninitialize_ = result_ == S_OK || result_ == S_FALSE;
  }

  ~ComApartment() {
    if (uninitialize_) {
      CoUninitialize();
    }
  }

  [[nodiscard]] HRESULT result() const noexcept { return result_; }

 private:
  HRESULT result_{E_FAIL};
  bool uninitialize_{};
};

template <typename T>
class ComPointer final {
 public:
  ComPointer() = default;
  ~ComPointer() {
    if (value_ != nullptr) {
      value_->Release();
    }
  }
  ComPointer(const ComPointer&) = delete;
  ComPointer& operator=(const ComPointer&) = delete;
  [[nodiscard]] T** put() noexcept { return &value_; }
  [[nodiscard]] T* get() const noexcept { return value_; }
  [[nodiscard]] T* operator->() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }

 private:
  T* value_{};
};

class UniqueBstr final {
 public:
  explicit UniqueBstr(const wchar_t* value) : value_(SysAllocString(value)) {}
  ~UniqueBstr() { SysFreeString(value_); }
  UniqueBstr(const UniqueBstr&) = delete;
  UniqueBstr& operator=(const UniqueBstr&) = delete;
  [[nodiscard]] BSTR get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }

 private:
  BSTR value_{};
};

clonecore::Error wmi_error(
    const std::wstring_view operation,
    const HRESULT result) {
  return check_error(
      clonecore::ErrorCode::query_failed,
      static_cast<DWORD>(result),
      std::wstring(operation),
      L"ローカルBitLocker状態の読取り専用照会に失敗しました");
}

clonecore::Result<std::uint32_t> get_wmi_uint32(
    IWbemClassObject& object,
    const wchar_t* property) {
  VARIANT value;
  VariantInit(&value);
  const HRESULT result = object.Get(property, 0, &value, nullptr, nullptr);
  if (FAILED(result)) {
    VariantClear(&value);
    return clonecore::Result<std::uint32_t>::failure(
        wmi_error(L"BitLocker WMI property取得", result));
  }
  std::optional<std::uint32_t> parsed;
  if (value.vt == VT_UI4) {
    parsed = value.ulVal;
  } else if (value.vt == VT_I4 && value.lVal >= 0) {
    parsed = static_cast<std::uint32_t>(value.lVal);
  } else if (value.vt == VT_UI2) {
    parsed = value.uiVal;
  } else if (value.vt == VT_I2 && value.iVal >= 0) {
    parsed = static_cast<std::uint32_t>(value.iVal);
  }
  VariantClear(&value);
  if (!parsed.has_value()) {
    return clonecore::Result<std::uint32_t>::failure(check_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"BitLocker WMI property形式",
        L"BitLocker WMI propertyが非負整数ではありません"));
  }
  return clonecore::Result<std::uint32_t>::success(parsed.value());
}

clonecore::Result<PostMigrationBitLockerObservation>
query_bitlocker_read_only(const std::wstring& drive_letter) {
  if (drive_letter.size() != 2U ||
      drive_letter[0] < L'A' || drive_letter[0] > L'Z' ||
      drive_letter[1] != L':') {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        check_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DRIVE,
            L"BitLocker照会ドライブ",
            L"現在Windowsの固定ドライブ文字を確認できません"));
  }

  ComApartment apartment;
  if (FAILED(apartment.result()) &&
      apartment.result() != RPC_E_CHANGED_MODE) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        wmi_error(L"BitLocker COM初期化", apartment.result()));
  }

  ComPointer<IWbemLocator> locator;
  HRESULT result = CoCreateInstance(
      CLSID_WbemLocator,
      nullptr,
      CLSCTX_INPROC_SERVER,
      IID_IWbemLocator,
      reinterpret_cast<void**>(locator.put()));
  if (FAILED(result) || !locator) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        wmi_error(L"BitLocker WMI locator作成", result));
  }

  UniqueBstr name_space(
      L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
  if (!name_space) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        check_error(
            clonecore::ErrorCode::internal_error,
            ERROR_OUTOFMEMORY,
            L"BitLocker WMI namespace",
            L"BitLocker WMI namespace文字列を確保できません"));
  }
  ComPointer<IWbemServices> services;
  result = locator->ConnectServer(
      name_space.get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr,
      services.put());
  if (result == WBEM_E_INVALID_NAMESPACE || result == WBEM_E_NOT_FOUND) {
    return clonecore::Result<PostMigrationBitLockerObservation>::success(
        PostMigrationBitLockerObservation{
            .conversion = PostMigrationBitLockerConversionState::
                provider_unavailable,
            .protection = PostMigrationBitLockerProtectionState::
                provider_unavailable,
        });
  }
  if (FAILED(result) || !services) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        wmi_error(L"BitLocker WMI namespace接続", result));
  }

  result = CoSetProxyBlanket(
      services.get(),
      RPC_C_AUTHN_WINNT,
      RPC_C_AUTHZ_NONE,
      nullptr,
      RPC_C_AUTHN_LEVEL_CALL,
      RPC_C_IMP_LEVEL_IMPERSONATE,
      nullptr,
      EOAC_NONE);
  if (FAILED(result)) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        wmi_error(L"BitLocker WMI proxy保護", result));
  }

  const std::wstring query_text =
      L"SELECT ConversionStatus, ProtectionStatus, EncryptionPercentage "
      L"FROM Win32_EncryptableVolume WHERE DriveLetter='" +
      drive_letter + L"'";
  UniqueBstr language(L"WQL");
  UniqueBstr query(query_text.c_str());
  if (!language || !query) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        check_error(
            clonecore::ErrorCode::internal_error,
            ERROR_OUTOFMEMORY,
            L"BitLocker WMI query",
            L"BitLocker WMI query文字列を確保できません"));
  }
  ComPointer<IEnumWbemClassObject> enumerator;
  result = services->ExecQuery(
      language.get(),
      query.get(),
      WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
      nullptr,
      enumerator.put());
  if (FAILED(result) || !enumerator) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        wmi_error(L"BitLocker WMI SELECT", result));
  }

  ComPointer<IWbemClassObject> row;
  ULONG returned = 0U;
  result = enumerator->Next(5000U, 1U, row.put(), &returned);
  if (result == static_cast<HRESULT>(WBEM_S_TIMEDOUT)) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        check_error(
            clonecore::ErrorCode::query_failed,
            ERROR_TIMEOUT,
            L"BitLocker WMI応答",
            L"BitLocker状態の照会が5秒以内に完了しませんでした"));
  }
  if (FAILED(result)) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        wmi_error(L"BitLocker WMI行取得", result));
  }
  if (returned == 0U || !row) {
    return clonecore::Result<PostMigrationBitLockerObservation>::success(
        PostMigrationBitLockerObservation{
            .conversion = PostMigrationBitLockerConversionState::
                provider_unavailable,
            .protection = PostMigrationBitLockerProtectionState::
                provider_unavailable,
        });
  }

  auto conversion = get_wmi_uint32(*row.get(), L"ConversionStatus");
  auto protection = get_wmi_uint32(*row.get(), L"ProtectionStatus");
  auto percentage = get_wmi_uint32(*row.get(), L"EncryptionPercentage");
  if (!conversion || !protection || !percentage) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        !conversion ? conversion.error()
                    : !protection ? protection.error()
                                  : percentage.error());
  }

  ComPointer<IWbemClassObject> extra;
  ULONG extra_returned = 0U;
  result = enumerator->Next(0U, 1U, extra.put(), &extra_returned);
  if (FAILED(result) || extra_returned != 0U) {
    return clonecore::Result<PostMigrationBitLockerObservation>::failure(
        check_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_DUP_NAME,
            L"BitLocker WMI対象一意性",
            L"現在WindowsのBitLocker WMI対象を一意に確認できません"));
  }

  PostMigrationBitLockerObservation observation;
  switch (conversion.value()) {
    case 0U:
      observation.conversion =
          PostMigrationBitLockerConversionState::fully_decrypted;
      break;
    case 1U:
      observation.conversion =
          PostMigrationBitLockerConversionState::fully_encrypted;
      break;
    case 2U:
      observation.conversion =
          PostMigrationBitLockerConversionState::encryption_in_progress;
      break;
    case 3U:
      observation.conversion =
          PostMigrationBitLockerConversionState::decryption_in_progress;
      break;
    case 4U:
      observation.conversion =
          PostMigrationBitLockerConversionState::encryption_paused;
      break;
    case 5U:
      observation.conversion =
          PostMigrationBitLockerConversionState::decryption_paused;
      break;
    default:
      observation.conversion =
          PostMigrationBitLockerConversionState::unknown;
      break;
  }
  switch (protection.value()) {
    case 0U:
      observation.protection =
          PostMigrationBitLockerProtectionState::off;
      break;
    case 1U:
      observation.protection =
          PostMigrationBitLockerProtectionState::on;
      break;
    default:
      observation.protection =
          PostMigrationBitLockerProtectionState::unknown;
      break;
  }
  if (percentage.value() <= 100U) {
    observation.encryption_percentage = percentage.value();
  }
  return clonecore::Result<PostMigrationBitLockerObservation>::success(
      observation);
}

std::wstring bitlocker_conversion_name(
    const PostMigrationBitLockerConversionState state) {
  switch (state) {
    case PostMigrationBitLockerConversionState::fully_decrypted:
      return L"完全復号";
    case PostMigrationBitLockerConversionState::fully_encrypted:
      return L"完全暗号化";
    case PostMigrationBitLockerConversionState::encryption_in_progress:
      return L"暗号化中";
    case PostMigrationBitLockerConversionState::decryption_in_progress:
      return L"復号中";
    case PostMigrationBitLockerConversionState::encryption_paused:
      return L"暗号化一時停止";
    case PostMigrationBitLockerConversionState::decryption_paused:
      return L"復号一時停止";
    case PostMigrationBitLockerConversionState::provider_unavailable:
      return L"取得不可";
    case PostMigrationBitLockerConversionState::unknown:
    default:
      return L"不明";
  }
}

std::wstring bitlocker_protection_name(
    const PostMigrationBitLockerProtectionState state) {
  switch (state) {
    case PostMigrationBitLockerProtectionState::off:
      return L"保護オフ";
    case PostMigrationBitLockerProtectionState::on:
      return L"保護オン";
    case PostMigrationBitLockerProtectionState::provider_unavailable:
      return L"取得不可";
    case PostMigrationBitLockerProtectionState::unknown:
    default:
      return L"不明";
  }
}

std::wstring evidence_state_name(const PostMigrationEvidenceState state) {
  switch (state) {
    case PostMigrationEvidenceState::verified:
      return L"PASS";
    case PostMigrationEvidenceState::attention:
      return L"WARN";
    case PostMigrationEvidenceState::unavailable:
    default:
      return L"NOT RUN";
  }
}

class WindowsPostMigrationReadOnlyPlatform final
    : public IPostMigrationReadOnlyPlatform {
 public:
  clonecore::Result<PostMigrationCheckReport> inspect_read_only() override {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    if (inventory == nullptr) {
      return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"移行後チェックのディスク列挙基盤",
          L"読取り専用ディスク列挙基盤を初期化できません"));
    }
    auto enumerated = inventory->enumerate();
    if (!enumerated) {
      return clonecore::Result<PostMigrationCheckReport>::failure(
          enumerated.error());
    }
    std::vector<const diskmodel::DiskInfo*> system_disks;
    for (const auto& disk : enumerated.value().disks) {
      if (disk.is_system_disk) {
        system_disks.push_back(&disk);
      }
    }
    if (system_disks.size() != 1U) {
      return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"現在の起動ディスク一意性",
          L"現在の起動ディスクを一台に特定できません"));
    }
    const diskmodel::DiskInfo boot_disk = *system_disks.front();
    if (boot_disk.offline.value_or(true) ||
        !boot_disk.read_only.has_value() ||
        boot_disk.partition_style == diskmodel::PartitionStyle::raw ||
        boot_disk.partition_style == diskmodel::PartitionStyle::unknown ||
        boot_disk.partitions.empty()) {
      return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"現在の起動ディスク状態",
          L"オンラインのGPT/MBR起動ディスクとして確認できません"));
    }

    const std::wstring windows_directory = get_windows_directory();
    if (windows_directory.empty()) {
      return clonecore::Result<PostMigrationCheckReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"現在Windowsディレクトリ取得",
              GetLastError()));
    }
    const auto windows_disk_number =
        diskmodel::query_single_disk_number_for_local_path(
            windows_directory);
    if (!windows_disk_number ||
        windows_disk_number.value() != boot_disk.disk_number) {
      return clonecore::Result<PostMigrationCheckReport>::failure(
          !windows_disk_number
              ? windows_disk_number.error()
              : check_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_NOT_SAME_DEVICE,
                    L"現在Windowsと起動ディスクの対応",
                    L"現在Windowsの物理ディスクと起動ディスク表示が一致しません"));
    }
    const auto current_volume = windows_volume_name(windows_directory);
    if (!current_volume) {
      return clonecore::Result<PostMigrationCheckReport>::failure(
          current_volume.error());
    }

    auto volume_result =
        bootrepair::enumerate_windows_boot_volumes_read_only();
    if (!volume_result) {
      return clonecore::Result<PostMigrationCheckReport>::failure(
          volume_result.error());
    }
    const auto& volumes = volume_result.value();
    const auto windows_volume_it = std::find_if(
        volumes.begin(),
        volumes.end(),
        [&](const bootrepair::BootVolumeObservation& volume) {
          return equals_case_insensitive(
              volume.volume_name, current_volume.value());
        });
    if (windows_volume_it == volumes.end() ||
        windows_volume_it->location.disk_number != boot_disk.disk_number) {
      return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"現在Windowsボリュームの物理対応",
          L"現在WindowsのVolume GUIDを起動ディスク上に一意に確認できません"));
    }
    const auto* windows_partition =
        find_partition_for_volume(boot_disk, *windows_volume_it);
    if (windows_partition == nullptr) {
      return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"現在Windowsパーティションの範囲",
          L"現在Windowsのボリューム範囲がパーティション表と一致しません"));
    }

    std::vector<std::pair<const diskmodel::PartitionInfo*,
                          const bootrepair::BootVolumeObservation*>>
        system_candidates;
    for (const auto& partition : boot_disk.partitions) {
      const bool structural_match =
          boot_disk.partition_style == diskmodel::PartitionStyle::gpt
          ? equals_case_insensitive(partition.type, kEfiPartitionType)
          : partition.bootable &&
              equals_case_insensitive(partition.type, kMbrNtfsPartitionType);
      if (!structural_match) {
        continue;
      }
      const auto* volume = find_volume_for_partition(
          volumes, boot_disk.disk_number, partition);
      if (volume != nullptr) {
        system_candidates.emplace_back(&partition, volume);
      }
    }
    if (system_candidates.size() != 1U) {
      return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_FOUND,
          L"現在のシステム領域一意性",
          L"ESPまたはBIOS Activeシステム領域を一つに特定できません"));
    }
    const auto& [system_partition, system_volume] =
        system_candidates.front();

    PostMigrationCheckReport report{
        .current_boot_disk = boot_disk,
        .windows_partition_number = windows_partition->number,
        .system_partition_number = system_partition->number,
        .windows_volume_name = windows_volume_it->volume_name,
        .windows_file_system = windows_volume_it->location.file_system,
        .system_volume_name = system_volume->volume_name,
        .system_file_system = system_volume->location.file_system,
        .read_only_operations_only = true,
        .preboot_success_guaranteed = false,
    };

    const bool expected_system_file_system =
        boot_disk.partition_style == diskmodel::PartitionStyle::gpt
        ? equals_case_insensitive(report.system_file_system, L"FAT32")
        : equals_case_insensitive(report.system_file_system, L"NTFS");
    const bool layout_verified =
        equals_case_insensitive(report.windows_file_system, L"NTFS") &&
        expected_system_file_system;
    report.boot_disk_and_layout = layout_verified
        ? verified_evidence(
              L"現在の起動ディスクと想定レイアウトを確認しました",
              L"現在WindowsのVolume GUID、物理ディスク、パーティション範囲、GPT/MBR方式、システム領域を読取り専用で照合しました。")
        : attention_evidence(
              L"想定レイアウトと異なる項目があります",
              L"現在WindowsはNTFS、GPTのESPはFAT32、MBRのActiveシステム領域はNTFSであることを確認してください。");

    const std::wstring bcd_path = append_volume_path(
        report.system_volume_name,
        boot_disk.partition_style == diskmodel::PartitionStyle::gpt
            ? L"EFI\\Microsoft\\Boot\\BCD"
            : L"Boot\\BCD");
    const auto bcd =
        bootrepair::verify_bcd_store_file_with_windows_apis(bcd_path);
    report.bcd_and_boot_manager = bcd
        ? verified_evidence(
              L"BCDとWindows Boot Managerを確認しました",
              L"現在のシステム領域にあるBCDを読取り専用registry hiveとして開き、ObjectsとWindows Boot Managerを確認しました。")
        : unavailable_evidence(
              L"BCDを完全には確認できませんでした",
              L"BCDまたはWindows Boot Managerの読取り専用検証に失敗しました。起動修復を自動実行してはいません。",
              bcd.error());

    inspect_winre(report, volumes);
    inspect_file_system_and_health(report, layout_verified);
    inspect_bitlocker(report, current_drive_letter(windows_directory));

    return clonecore::Result<PostMigrationCheckReport>::success(
        std::move(report));
  }

 private:
  static void inspect_winre(
      PostMigrationCheckReport& report,
      const std::vector<bootrepair::BootVolumeObservation>& volumes) {
    std::vector<wchar_t> system_directory(32768U, L'\0');
    const UINT length = GetSystemDirectoryW(
        system_directory.data(),
        static_cast<UINT>(system_directory.size()));
    if (length == 0U || length >= system_directory.size()) {
      const auto error = clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"WinRE診断System32取得",
          GetLastError());
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE状態を確認できませんでした",
          L"現在WindowsのSystem32を取得できません。WinRE設定は変更していません。",
          error);
      return;
    }
    auto trust = bootrepair::make_windows_authenticode_verifier();
    auto runner = bootrepair::make_windows_process_runner(30U * 1000U);
    if (trust == nullptr || runner == nullptr) {
      const auto error = check_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"WinRE読取り専用診断基盤",
          L"署名検証またはプロセス実行基盤を初期化できません");
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE状態を確認できませんでした",
          L"読取り専用診断基盤を初期化できません。WinRE設定は変更していません。",
          error);
      return;
    }
    const std::wstring system_root(system_directory.data(), length);
    const std::wstring reagentc = system_root + L"\\reagentc.exe";
    const auto signed_binary = trust->verify_microsoft_signed(reagentc);
    if (!signed_binary) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE状態を確認できませんでした",
          L"Microsoft署名済みREAgentCを確認できないため実行していません。",
          signed_binary.error());
      return;
    }
    const auto process = runner->run(
        reagentc, {L"/info"}, system_root);
    if (!process) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE状態を確認できませんでした",
          L"署名済みREAgentC /infoの読取り専用実行に失敗しました。",
          process.error());
      return;
    }
    if (process.value().exit_code != 0U) {
      report.winre_state = PostMigrationWinReState::disabled_or_missing;
      report.winre = attention_evidence(
          L"WinREは無効または確認不能です",
          L"REAgentC /infoが成功しませんでした。設定変更や自動修復は行っていません。");
      return;
    }
    const auto registered = bootrepair::parse_reagentc_registered_location(
        process.value().standard_output);
    if (!registered) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE登録先を解析できませんでした",
          L"REAgentC /infoの登録先を一意に解析できません。設定変更は行っていません。",
          registered.error());
      return;
    }
    if (!registered.value().has_value()) {
      report.winre_state = PostMigrationWinReState::disabled_or_missing;
      report.winre = attention_evidence(
          L"WinRE登録先がありません",
          L"REAgentC /infoに対応する登録先がありません。自動修復は行っていません。");
      return;
    }
    const auto& location = registered.value().value();
    if (location.disk_number != report.current_boot_disk.disk_number) {
      report.winre_state =
          PostMigrationWinReState::registered_on_another_disk;
      report.winre = attention_evidence(
          L"WinREが別ディスクを参照しています",
          L"現在の起動ディスクとREAgentC登録先ディスクが一致しません。自動修復は行っていません。");
      return;
    }
    const auto partition = std::find_if(
        report.current_boot_disk.partitions.begin(),
        report.current_boot_disk.partitions.end(),
        [&](const diskmodel::PartitionInfo& candidate) {
          return candidate.number == location.partition_number;
        });
    if (partition == report.current_boot_disk.partitions.end()) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = attention_evidence(
          L"WinRE登録先パーティションを確認できません",
          L"REAgentC登録先が現在のパーティション表にありません。自動修復は行っていません。");
      return;
    }
    if (location.path_kind ==
            bootrepair::WinReRegisteredPathKind::
                windows_system32_recovery &&
        location.partition_number != report.windows_partition_number) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = attention_evidence(
          L"WinREのWindows内登録先が一致しません",
          L"Windows\\System32\\Recoveryの登録先パーティションが現在Windows区画と一致しません。自動修復は行っていません。");
      return;
    }
    const auto* volume = find_volume_for_partition(
        volumes, report.current_boot_disk.disk_number, *partition);
    if (volume == nullptr) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = attention_evidence(
          L"WinRE登録先ボリュームを確認できません",
          L"REAgentC登録先のVolume GUIDを読取り専用で特定できません。自動修復は行っていません。");
      return;
    }
    const std::wstring image_path =
        location.path_kind ==
                bootrepair::WinReRegisteredPathKind::recovery_windows_re
        ? append_volume_path(
              volume->volume_name, L"Recovery\\WindowsRE\\Winre.wim")
        : append_volume_path(
              report.windows_volume_name,
              L"Windows\\System32\\Recovery\\Winre.wim");
    auto image_probe = bootrepair::make_windows_winre_image_probe();
    if (image_probe == nullptr) {
      const auto error = check_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"WinRE image読取り専用基盤",
          L"WinRE image検証基盤を初期化できません");
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE imageを確認できませんでした",
          L"WinRE imageの読取り専用検証基盤を初期化できません。",
          error);
      return;
    }
    const auto image = image_probe->inspect_regular_image(image_path);
    if (!image) {
      report.winre_state = PostMigrationWinReState::unknown;
      report.winre = unavailable_evidence(
          L"WinRE imageを確認できませんでした",
          L"登録先Winre.wimを通常・非reparseファイルとして確認できません。",
          image.error());
      return;
    }
    if (!image.value().exists || image.value().length == 0U) {
      report.winre_state = PostMigrationWinReState::disabled_or_missing;
      report.winre = attention_evidence(
          L"WinRE imageが見つかりません",
          L"登録先のWinre.wimがないか空です。自動修復は行っていません。");
      return;
    }
    report.winre_state = PostMigrationWinReState::registered;
    report.winre = verified_evidence(
        L"WinRE登録と回復領域を確認しました",
        L"Microsoft署名済みREAgentC /infoの登録先が現在の起動ディスクと一致し、Winre.wimを読取り専用で確認しました。");
  }

  static void inspect_file_system_and_health(
      PostMigrationCheckReport& report,
      const bool layout_verified) {
    const auto state = report.current_boot_disk.health.state;
    const bool health_verified =
        state == diskmodel::DiskHealthState::healthy;
    const bool health_attention =
        state == diskmodel::DiskHealthState::caution ||
        state == diskmodel::DiskHealthState::failing;
    if (layout_verified && health_verified) {
      report.file_system_and_disk_health = verified_evidence(
          L"ファイルシステムとディスク健康状態を確認しました",
          L"Windows/システム領域のファイルシステムが想定どおりで、SMARTまたはNVMe健康情報は正常です。温度は警告表示だけに使用します。");
      return;
    }
    std::wstring detail = layout_verified
        ? L"ファイルシステムは想定どおりです。"
        : L"Windowsまたはシステム領域のファイルシステムが想定と異なります。";
    if (health_attention) {
      detail += state == diskmodel::DiskHealthState::failing
          ? L" ディスク健康情報は故障予測を示しています。"
          : L" ディスク健康情報に注意項目があります。";
    } else {
      detail += L" SMART/NVMe健康情報を取得できませんでした。";
    }
    report.file_system_and_disk_health = attention_evidence(
        L"ファイルシステムまたは健康状態に確認項目があります",
        std::move(detail));
  }

  static void inspect_bitlocker(
      PostMigrationCheckReport& report,
      const std::wstring& drive_letter) {
    const auto bitlocker = query_bitlocker_read_only(drive_letter);
    if (!bitlocker) {
      report.bitlocker_status = unavailable_evidence(
          L"BitLocker状態を確認できませんでした",
          L"ローカルWMIの読取り専用SELECTに失敗しました。保護状態は変更していません。",
          bitlocker.error());
      return;
    }
    report.bitlocker = bitlocker.value();
    if (report.bitlocker.conversion ==
            PostMigrationBitLockerConversionState::provider_unavailable ||
        report.bitlocker.protection ==
            PostMigrationBitLockerProtectionState::provider_unavailable) {
      report.bitlocker_status = attention_evidence(
          L"BitLocker providerから状態を取得できません",
          L"BitLocker状態は不明です。暗号化や保護設定の変更は行っていません。");
      return;
    }
    const bool transient =
        report.bitlocker.conversion ==
            PostMigrationBitLockerConversionState::encryption_in_progress ||
        report.bitlocker.conversion ==
            PostMigrationBitLockerConversionState::decryption_in_progress ||
        report.bitlocker.conversion ==
            PostMigrationBitLockerConversionState::encryption_paused ||
        report.bitlocker.conversion ==
            PostMigrationBitLockerConversionState::decryption_paused ||
        report.bitlocker.conversion ==
            PostMigrationBitLockerConversionState::unknown ||
        report.bitlocker.protection ==
            PostMigrationBitLockerProtectionState::unknown;
    std::wstring detail = L"変換状態: " +
        bitlocker_conversion_name(report.bitlocker.conversion) +
        L" / 保護状態: " +
        bitlocker_protection_name(report.bitlocker.protection);
    if (report.bitlocker.encryption_percentage.has_value()) {
      detail += L" / 暗号化率: " +
          std::to_wstring(report.bitlocker.encryption_percentage.value()) +
          L"%";
    }
    report.bitlocker_status = transient
        ? attention_evidence(
              L"BitLocker状態に確認項目があります", std::move(detail))
        : verified_evidence(
              L"BitLocker状態を確認しました", std::move(detail));
  }
};

}  // namespace

clonecore::Result<PostMigrationCheckReport>
validate_post_migration_check_report(PostMigrationCheckReport report) {
  const bool disk_valid = report.current_boot_disk.is_system_disk &&
      report.current_boot_disk.size_bytes != 0U &&
      !report.current_boot_disk.partitions.empty() &&
      (report.current_boot_disk.partition_style ==
           diskmodel::PartitionStyle::gpt ||
       report.current_boot_disk.partition_style ==
           diskmodel::PartitionStyle::mbr) &&
      valid_partition_number(
          report.current_boot_disk, report.windows_partition_number) &&
      valid_partition_number(
          report.current_boot_disk, report.system_partition_number) &&
      valid_volume_name(report.windows_volume_name) &&
      valid_volume_name(report.system_volume_name) &&
      valid_file_system(report.windows_file_system) &&
      valid_file_system(report.system_file_system);
  const bool evidence_valid =
      valid_evidence(report.boot_disk_and_layout) &&
      valid_evidence(report.bcd_and_boot_manager) &&
      valid_evidence(report.winre) &&
      valid_evidence(report.file_system_and_disk_health) &&
      valid_evidence(report.bitlocker_status);
  const bool percentage_valid =
      !report.bitlocker.encryption_percentage.has_value() ||
      report.bitlocker.encryption_percentage.value() <= 100U;
  if (!disk_valid || !evidence_valid || !percentage_valid ||
      !report.read_only_operations_only ||
      report.preboot_success_guaranteed) {
    return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"移行後チェック結果の契約検証",
        L"起動ディスク、ボリューム、証拠、BitLocker値、読取り専用境界、または事前起動保証表示が不正です"));
  }
  return clonecore::Result<PostMigrationCheckReport>::success(
      std::move(report));
}

clonecore::Result<PostMigrationCheckReport>
run_post_migration_check(IPostMigrationReadOnlyPlatform& platform) {
  auto report = platform.inspect_read_only();
  if (!report) {
    return clonecore::Result<PostMigrationCheckReport>::failure(
        report.error());
  }
  return validate_post_migration_check_report(report.take_value());
}

std::unique_ptr<IPostMigrationReadOnlyPlatform>
make_windows_post_migration_read_only_platform() {
  return std::make_unique<WindowsPostMigrationReadOnlyPlatform>();
}

clonecore::Result<PostMigrationCheckReport>
run_post_migration_check_with_windows_apis() {
  auto platform = make_windows_post_migration_read_only_platform();
  if (platform == nullptr) {
    return clonecore::Result<PostMigrationCheckReport>::failure(check_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"移行後チェックWindows基盤",
        L"移行後チェックの読取り専用Windows基盤を初期化できません"));
  }
  return run_post_migration_check(*platform);
}

std::wstring format_post_migration_check_report(
    const PostMigrationCheckReport& report) {
  std::wostringstream output;
  output << L"換装後の読取り専用チェック\r\n"
         << L"※これは現在の起動後診断です。換装前の起動成功を保証しません。\r\n\r\n"
         << L"起動ディスク: Disk " << report.current_boot_disk.disk_number
         << L" / " << report.current_boot_disk.model << L"\r\n"
         << L"方式: "
         << diskmodel::partition_style_name(
                report.current_boot_disk.partition_style)
         << L" / Windows区画: " << report.windows_partition_number
         << L" (" << report.windows_file_system << L")"
         << L" / システム区画: " << report.system_partition_number
         << L" (" << report.system_file_system << L")\r\n\r\n";

  const auto append = [&](const PostMigrationEvidence& evidence) {
    output << L"[" << evidence_state_name(evidence.state) << L"] "
           << evidence.summary << L"\r\n  " << evidence.detail;
    if (evidence.error.has_value()) {
      output << L"\r\n  Code: "
             << clonecore::error_code_name(evidence.error->code)
             << L" / Native: " << evidence.error->native_code;
    }
    output << L"\r\n";
  };
  append(report.boot_disk_and_layout);
  append(report.bcd_and_boot_manager);
  append(report.winre);
  append(report.file_system_and_disk_health);
  append(report.bitlocker_status);
  return output.str();
}

}  // namespace ytec::windowsapp

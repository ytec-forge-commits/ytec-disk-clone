#include "ytec/bootrepair/system_partition_creation.h"

#include "ytec/bootrepair/automatic_repair_windows.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>
#include <vds.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::bootrepair {
namespace {

constexpr GUID kVdsLoaderClassId{
    0x9c38ed61,
    0xd565,
    0x4728,
    {0xae, 0xee, 0xc8, 0x09, 0x52, 0xf0, 0xec, 0xde}};
constexpr GUID kEfiSystemPartitionType{
    0xc12a7328,
    0xf81f,
    0x11d2,
    {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};
constexpr ULONG kForbiddenWindowsVolumeFlags =
    VDS_VF_SYSTEM_VOLUME | VDS_VF_BOOT_VOLUME | VDS_VF_PAGEFILE |
    VDS_VF_HIBERNATION | VDS_VF_CRASHDUMP | VDS_VF_SHADOW_COPY |
    VDS_VF_FVE_ENABLED | VDS_VF_BACKS_BOOT_VOLUME |
    VDS_VF_BACKED_BY_WIM_IMAGE;

clonecore::Error windows_creation_error(
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
  return clonecore::Result<T>::failure(windows_creation_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(windows_creation_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool text_equal(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(), left.end(), right.begin(),
          [](const wchar_t l, const wchar_t r) {
            return std::towlower(l) == std::towlower(r);
          });
}

std::wstring normalize_volume_name(std::wstring value) {
  while (value.size() > 1U && value.back() == L'\\') {
    value.pop_back();
  }
  return value;
}

template <typename T>
class ComPtr final {
 public:
  ComPtr() = default;
  explicit ComPtr(T* value) noexcept : value_(value) {}
  ~ComPtr() {
    if (value_ != nullptr) {
      value_->Release();
    }
  }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) {
        value_->Release();
      }
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  [[nodiscard]] T* get() const noexcept { return value_; }
  [[nodiscard]] T** put() noexcept {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
    return &value_;
  }
  [[nodiscard]] T* operator->() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr;
  }

 private:
  T* value_{};
};

class ComInitialization final {
 public:
  ComInitialization() {
    result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninitialize_ = result_ == S_OK || result_ == S_FALSE;
  }
  ~ComInitialization() {
    if (uninitialize_) {
      CoUninitialize();
    }
  }
  [[nodiscard]] HRESULT result() const noexcept { return result_; }

 private:
  HRESULT result_{E_FAIL};
  bool uninitialize_{};
};

void free_disk_properties(VDS_DISK_PROP& properties) noexcept {
  CoTaskMemFree(properties.pwszDiskAddress);
  CoTaskMemFree(properties.pwszName);
  CoTaskMemFree(properties.pwszFriendlyName);
  CoTaskMemFree(properties.pwszAdaptorName);
  CoTaskMemFree(properties.pwszDevicePath);
  properties = {};
}

void free_advanced_disk_properties(
    VDS_ADVANCEDDISK_PROP& properties) noexcept {
  CoTaskMemFree(properties.pwszId);
  CoTaskMemFree(properties.pwszPathname);
  CoTaskMemFree(properties.pwszLocation);
  CoTaskMemFree(properties.pwszFriendlyName);
  CoTaskMemFree(properties.pswzIdentifier);
  CoTaskMemFree(properties.pwszSerialNumber);
  CoTaskMemFree(properties.pwszFirmwareVersion);
  CoTaskMemFree(properties.pwszManufacturer);
  CoTaskMemFree(properties.pwszModel);
  properties = {};
}

struct VdsDiskBinding final {
  ComPtr<IVdsDisk> disk;
  ComPtr<IVdsAdvancedDisk> advanced;
  ComPtr<IVdsAdvancedDisk3> advanced_properties;
  VDS_OBJECT_ID id{};
};

bool vds_disk_properties_match(
    const diskmodel::DiskInfo& expected,
    const VDS_DISK_PROP& disk,
    const VDS_ADVANCEDDISK_PROP& advanced) noexcept {
  return advanced.ulNumber == expected.disk_number &&
      advanced.ullTotalSize == expected.size_bytes &&
      advanced.ulLogicalSectorSize == expected.logical_sector_size &&
      advanced.PartitionStyle ==
          (expected.partition_style == diskmodel::PartitionStyle::gpt
               ? VDS_PST_GPT
               : VDS_PST_MBR) &&
      advanced.status == VDS_DS_ONLINE &&
      advanced.health == VDS_H_HEALTHY && disk.status == VDS_DS_ONLINE &&
      disk.health == VDS_H_HEALTHY;
}

clonecore::Status revalidate_vds_disk_binding_exact(
    const VdsDiskBinding& binding,
    const diskmodel::DiskInfo& expected) {
  if (!binding.disk || !binding.advanced || !binding.advanced_properties) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_HANDLE,
        L"起動修復システム領域作成のVDS object再拘束",
        L"reviewed VDS disk objectの全interfaceを保持できていません");
  }
  VDS_DISK_PROP disk_properties{};
  VDS_ADVANCEDDISK_PROP advanced_properties{};
  const HRESULT disk_result =
      binding.disk->GetProperties(&disk_properties);
  const HRESULT advanced_result =
      binding.advanced_properties->GetProperties(&advanced_properties);
  const bool matches = disk_result == S_OK && advanced_result == S_OK &&
      vds_disk_properties_match(expected, disk_properties, advanced_properties);
  free_disk_properties(disk_properties);
  free_advanced_disk_properties(advanced_properties);
  if (disk_result != S_OK || advanced_result != S_OK) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(
            disk_result != S_OK ? disk_result : advanced_result),
        L"起動修復システム領域作成のVDS object再照合",
        L"mutation直前にobject-bound VDS disk属性を取得できません");
  }
  return matches
      ? clonecore::success_status()
      : status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"起動修復システム領域作成のVDS object再照合",
            L"mutation直前のdisk番号、容量、sector、形式、状態が再解析結果と一致しません");
}

class VdsSession final {
 public:
  static clonecore::Result<std::unique_ptr<VdsSession>> open() {
    auto session = std::unique_ptr<VdsSession>(new VdsSession());
    const HRESULT initialized = session->com_.result();
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
      return failure<std::unique_ptr<VdsSession>>(
          clonecore::ErrorCode::unsupported_platform,
          static_cast<DWORD>(initialized),
          L"起動修復システム領域作成のVDS COM初期化",
          L"Microsoft Virtual Disk Serviceを初期化できません");
    }
    HRESULT result = CoCreateInstance(
        kVdsLoaderClassId,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(session->loader_.put()));
    if (result != S_OK || !session->loader_) {
      return failure<std::unique_ptr<VdsSession>>(
          clonecore::ErrorCode::unsupported_platform,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のVDS loader",
          L"Microsoft VDS loaderを取得できません");
    }
    result = session->loader_->LoadService(nullptr, session->service_.put());
    if (result != S_OK || !session->service_) {
      return failure<std::unique_ptr<VdsSession>>(
          clonecore::ErrorCode::unsupported_platform,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のVDS service",
          L"Microsoft VDS serviceを読み込めません");
    }
    result = session->service_->WaitForServiceReady();
    if (result != S_OK) {
      return failure<std::unique_ptr<VdsSession>>(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のVDS準備",
          L"Microsoft VDS serviceの準備完了を確認できません");
    }
    const auto refreshed = session->refresh();
    if (!refreshed) {
      return clonecore::Result<std::unique_ptr<VdsSession>>::failure(
          refreshed.error());
    }
    return clonecore::Result<std::unique_ptr<VdsSession>>::success(
        std::move(session));
  }

  clonecore::Status refresh() {
    HRESULT result = service_->Reenumerate();
    if (result == S_OK) {
      result = service_->Refresh();
    }
    return result == S_OK
        ? clonecore::success_status()
        : status_failure(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS再列挙",
              L"VDSのvolume/disk情報を更新できません");
  }

  clonecore::Result<ComPtr<IVdsVolume>> find_volume_exact(
      const std::wstring& expected_name) {
    const std::wstring normalized_expected =
        normalize_volume_name(expected_name);
    ComPtr<IVdsVolume> match;
    auto providers = query_providers();
    if (!providers) {
      return clonecore::Result<ComPtr<IVdsVolume>>::failure(
          providers.error());
    }
    while (true) {
      IUnknown* provider_raw = nullptr;
      ULONG count = 0U;
      HRESULT result = providers.value()->Next(
          1U, &provider_raw, &count);
      if (result == S_FALSE && count == 0U) {
        break;
      }
      if (result != S_OK || count != 1U || provider_raw == nullptr) {
        return failure<ComPtr<IVdsVolume>>(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"起動修復システム領域作成のVDS provider列挙",
            L"software provider列挙結果が一意ではありません");
      }
      ComPtr<IUnknown> provider_unknown(provider_raw);
      ComPtr<IVdsSwProvider> provider;
      result = provider_unknown->QueryInterface(
          IID_PPV_ARGS(provider.put()));
      if (result != S_OK || !provider) {
        return failure<ComPtr<IVdsVolume>>(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"起動修復システム領域作成のVDS provider照合",
            L"列挙objectをsoftware providerとして確認できません");
      }
      ComPtr<IEnumVdsObject> packs;
      result = provider->QueryPacks(packs.put());
      if (result != S_OK || !packs) {
        return failure<ComPtr<IVdsVolume>>(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"起動修復システム領域作成のVDS pack列挙",
            L"software providerのpackを列挙できません");
      }
      while (true) {
        IUnknown* pack_raw = nullptr;
        count = 0U;
        result = packs->Next(1U, &pack_raw, &count);
        if (result == S_FALSE && count == 0U) {
          break;
        }
        if (result != S_OK || count != 1U || pack_raw == nullptr) {
          return failure<ComPtr<IVdsVolume>>(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS pack取得",
              L"VDS pack列挙結果が一意ではありません");
        }
        ComPtr<IUnknown> pack_unknown(pack_raw);
        ComPtr<IVdsPack> pack;
        result = pack_unknown->QueryInterface(IID_PPV_ARGS(pack.put()));
        if (result != S_OK || !pack) {
          return failure<ComPtr<IVdsVolume>>(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS pack照合",
              L"列挙objectをVDS packとして確認できません");
        }
        ComPtr<IEnumVdsObject> volumes;
        result = pack->QueryVolumes(volumes.put());
        if (result != S_OK || !volumes) {
          return failure<ComPtr<IVdsVolume>>(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS volume列挙",
              L"VDS volumeを列挙できません");
        }
        while (true) {
          IUnknown* volume_raw = nullptr;
          count = 0U;
          result = volumes->Next(1U, &volume_raw, &count);
          if (result == S_FALSE && count == 0U) {
            break;
          }
          if (result != S_OK || count != 1U || volume_raw == nullptr) {
            return failure<ComPtr<IVdsVolume>>(
                clonecore::ErrorCode::query_failed,
                static_cast<DWORD>(result),
                L"起動修復システム領域作成のVDS volume取得",
                L"VDS volume列挙結果が一意ではありません");
          }
          ComPtr<IUnknown> volume_unknown(volume_raw);
          ComPtr<IVdsVolume> volume;
          result = volume_unknown->QueryInterface(
              IID_PPV_ARGS(volume.put()));
          if (result != S_OK || !volume) {
            return failure<ComPtr<IVdsVolume>>(
                clonecore::ErrorCode::query_failed,
                static_cast<DWORD>(result),
                L"起動修復システム領域作成のVDS volume照合",
                L"列挙objectをVDS volumeとして確認できません");
          }
          VDS_VOLUME_PROP properties{};
          result = volume->GetProperties(&properties);
          const std::wstring name =
              result == S_OK && properties.pwszName != nullptr
              ? normalize_volume_name(properties.pwszName)
              : L"";
          CoTaskMemFree(properties.pwszName);
          if (result != S_OK) {
            return failure<ComPtr<IVdsVolume>>(
                clonecore::ErrorCode::query_failed,
                static_cast<DWORD>(result),
                L"起動修復システム領域作成のVDS volume属性",
                L"VDS volume属性を取得できません");
          }
          if (!text_equal(name, normalized_expected)) {
            continue;
          }
          if (match) {
            return failure<ComPtr<IVdsVolume>>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DUP_NAME,
                L"起動修復システム領域作成のVDS volume一意性",
                L"同じVolume GUIDが複数のVDS objectへ対応しました");
          }
          match = std::move(volume);
        }
      }
    }
    if (!match) {
      return failure<ComPtr<IVdsVolume>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"起動修復システム領域作成のVDS volume検索",
          L"レビュー済みWindows Volume GUIDをVDSで一意に確認できません");
    }
    return clonecore::Result<ComPtr<IVdsVolume>>::success(
        std::move(match));
  }

  clonecore::Result<VdsDiskBinding> find_disk_exact(
      const diskmodel::DiskInfo& expected) {
    std::optional<VdsDiskBinding> match;
    auto providers = query_providers();
    if (!providers) {
      return clonecore::Result<VdsDiskBinding>::failure(providers.error());
    }
    while (true) {
      IUnknown* provider_raw = nullptr;
      ULONG count = 0U;
      HRESULT result = providers.value()->Next(
          1U, &provider_raw, &count);
      if (result == S_FALSE && count == 0U) {
        break;
      }
      if (result != S_OK || count != 1U || provider_raw == nullptr) {
        return failure<VdsDiskBinding>(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"起動修復システム領域作成のVDS disk provider列挙",
            L"software provider列挙結果が一意ではありません");
      }
      ComPtr<IUnknown> provider_unknown(provider_raw);
      ComPtr<IVdsSwProvider> provider;
      result = provider_unknown->QueryInterface(IID_PPV_ARGS(provider.put()));
      if (result != S_OK || !provider) {
        return failure<VdsDiskBinding>(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"起動修復システム領域作成のVDS disk provider照合",
            L"列挙objectをsoftware providerとして確認できません");
      }
      ComPtr<IEnumVdsObject> packs;
      result = provider->QueryPacks(packs.put());
      if (result != S_OK || !packs) {
        return failure<VdsDiskBinding>(
            clonecore::ErrorCode::query_failed,
            static_cast<DWORD>(result),
            L"起動修復システム領域作成のVDS disk pack列挙",
            L"software providerのpackを列挙できません");
      }
      while (true) {
        IUnknown* pack_raw = nullptr;
        count = 0U;
        result = packs->Next(1U, &pack_raw, &count);
        if (result == S_FALSE && count == 0U) {
          break;
        }
        if (result != S_OK || count != 1U || pack_raw == nullptr) {
          return failure<VdsDiskBinding>(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS disk pack取得",
              L"VDS pack列挙結果が一意ではありません");
        }
        ComPtr<IUnknown> pack_unknown(pack_raw);
        ComPtr<IVdsPack> pack;
        result = pack_unknown->QueryInterface(IID_PPV_ARGS(pack.put()));
        if (result != S_OK || !pack) {
          return failure<VdsDiskBinding>(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS disk pack照合",
              L"列挙objectをVDS packとして確認できません");
        }
        ComPtr<IEnumVdsObject> disks;
        result = pack->QueryDisks(disks.put());
        if (result != S_OK || !disks) {
          return failure<VdsDiskBinding>(
              clonecore::ErrorCode::query_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域作成のVDS disk列挙",
              L"VDS diskを列挙できません");
        }
        while (true) {
          IUnknown* disk_raw = nullptr;
          count = 0U;
          result = disks->Next(1U, &disk_raw, &count);
          if (result == S_FALSE && count == 0U) {
            break;
          }
          if (result != S_OK || count != 1U || disk_raw == nullptr) {
            return failure<VdsDiskBinding>(
                clonecore::ErrorCode::query_failed,
                static_cast<DWORD>(result),
                L"起動修復システム領域作成のVDS disk取得",
                L"VDS disk列挙結果が一意ではありません");
          }
          ComPtr<IUnknown> disk_unknown(disk_raw);
          ComPtr<IVdsDisk> disk;
          ComPtr<IVdsAdvancedDisk> advanced;
          ComPtr<IVdsAdvancedDisk3> advanced_properties_api;
          result = disk_unknown->QueryInterface(IID_PPV_ARGS(disk.put()));
          if (result == S_OK && disk) {
            result = disk_unknown->QueryInterface(
                IID_PPV_ARGS(advanced.put()));
          }
          if (result == S_OK && advanced) {
            result = disk_unknown->QueryInterface(
                IID_PPV_ARGS(advanced_properties_api.put()));
          }
          if (result != S_OK || !disk || !advanced ||
              !advanced_properties_api) {
            return failure<VdsDiskBinding>(
                clonecore::ErrorCode::query_failed,
                static_cast<DWORD>(result),
                L"起動修復システム領域作成のVDS advanced disk照合",
                L"列挙objectをbasic advanced diskとして確認できません");
          }
          VDS_DISK_PROP disk_properties{};
          VDS_ADVANCEDDISK_PROP advanced_properties{};
          HRESULT disk_result = disk->GetProperties(&disk_properties);
          HRESULT advanced_result =
              advanced_properties_api->GetProperties(&advanced_properties);
          const bool matches = disk_result == S_OK &&
              advanced_result == S_OK && vds_disk_properties_match(
                  expected, disk_properties, advanced_properties);
          const VDS_OBJECT_ID disk_id = disk_properties.id;
          free_disk_properties(disk_properties);
          free_advanced_disk_properties(advanced_properties);
          if (disk_result != S_OK || advanced_result != S_OK) {
            return failure<VdsDiskBinding>(
                clonecore::ErrorCode::query_failed,
                static_cast<DWORD>(
                    disk_result != S_OK ? disk_result : advanced_result),
                L"起動修復システム領域作成のVDS disk属性",
                L"VDS disk属性を取得できません");
          }
          if (!matches) {
            continue;
          }
          if (match.has_value()) {
            return failure<VdsDiskBinding>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DUP_NAME,
                L"起動修復システム領域作成のVDS disk一意性",
                L"現在の番号、容量、sector、形式へ複数diskが一致しました");
          }
          match.emplace(VdsDiskBinding{
              .disk = std::move(disk),
              .advanced = std::move(advanced),
              .advanced_properties = std::move(advanced_properties_api),
              .id = disk_id,
          });
        }
      }
    }
    if (!match.has_value()) {
      return failure<VdsDiskBinding>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"起動修復システム領域作成のVDS disk検索",
          L"安定再識別したdiskをVDSで一意に確認できません");
    }
    return clonecore::Result<VdsDiskBinding>::success(
        std::move(match.value()));
  }

  [[nodiscard]] IVdsService& service() const noexcept {
    return *service_.get();
  }

 private:
  VdsSession() = default;

  clonecore::Result<ComPtr<IEnumVdsObject>> query_providers() {
    ComPtr<IEnumVdsObject> providers;
    const HRESULT result = service_->QueryProviders(
        VDS_QUERY_SOFTWARE_PROVIDERS, providers.put());
    if (result != S_OK || !providers) {
      return failure<ComPtr<IEnumVdsObject>>(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のVDS provider取得",
          L"Microsoft software providerを列挙できません");
    }
    return clonecore::Result<ComPtr<IEnumVdsObject>>::success(
        std::move(providers));
  }

  ComInitialization com_;
  ComPtr<IVdsServiceLoader> loader_;
  ComPtr<IVdsService> service_;
};

clonecore::Status wait_async(
    IVdsAsync& operation,
    const VDS_ASYNC_OUTPUT_TYPE expected_type,
    VDS_ASYNC_OUTPUT& output,
    const std::wstring& name) {
  HRESULT operation_result = E_FAIL;
  const HRESULT waited = operation.Wait(&operation_result, &output);
  if (waited != S_OK || operation_result != S_OK ||
      output.type != expected_type) {
    return status_failure(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(waited != S_OK ? waited : operation_result),
        name,
        L"Microsoft VDS asynchronous operationを完了確認できません");
  }
  return clonecore::success_status();
}

clonecore::Status revalidate_vds_windows_volume_exact(
    IVdsVolume& volume,
    const std::uint64_t expected_size_bytes,
    const std::wstring& operation) {
  VDS_VOLUME_PROP properties{};
  const HRESULT result = volume.GetProperties(&properties);
  CoTaskMemFree(properties.pwszName);
  if (result != S_OK) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        operation,
        L"mutation直前にobject-bound Windows volume属性を再取得できません");
  }
  const bool matches = properties.ullSize == expected_size_bytes &&
      properties.type == VDS_VT_SIMPLE &&
      properties.RecommendedFileSystemType == VDS_FST_NTFS &&
      properties.status == VDS_VS_ONLINE &&
      properties.TransitionState == VDS_TS_STABLE &&
      properties.health == VDS_H_HEALTHY &&
      (properties.ulFlags & kForbiddenWindowsVolumeFlags) == 0U;
  return matches
      ? clonecore::success_status()
      : status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            operation,
            L"object-bound Windows volumeのsize、simple NTFS、状態、health、または保護roleがレビュー済み値と一致しません");
}

const DiscoveredWindowsInstallation* find_windows(
    const AutomaticBootRepairPlan& plan,
    const std::uint32_t partition_number) noexcept {
  const auto found = std::find_if(
      plan.windows_installations.begin(),
      plan.windows_installations.end(),
      [partition_number](const auto& value) {
        return value.partition.number == partition_number;
      });
  return found == plan.windows_installations.end() ? nullptr : &*found;
}

clonecore::Result<AutomaticBootRepairPlan> fresh_plan(
    const clonecore::StableDiskIdentity& expected) {
  auto planner = make_windows_automatic_boot_repair_plan_service();
  if (planner == nullptr) {
    return failure<AutomaticBootRepairPlan>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"起動修復システム領域作成の再解析",
        L"read-only automatic repair plannerを初期化できません");
  }
  return planner->plan(expected);
}

clonecore::Status validate_shrunken_binding(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& plan) {
  return validate_system_partition_creation_shrunken_plan(reviewed, plan);
}

clonecore::Status validate_completed_binding(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& plan) {
  return validate_system_partition_creation_completed_plan(reviewed, plan);
}

clonecore::Result<diskmodel::DiskInfo> observe_raw_disk_exact(
    const clonecore::StableDiskIdentity& expected) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (inventory == nullptr) {
    return failure<diskmodel::DiskInfo>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"起動修復システム領域作成のraw inventory初期化",
        L"失敗後cleanup用の読取り専用disk inventoryを初期化できません");
  }
  auto rebound = diskmodel::reidentify_read_only_physical_disk(
      expected, *inventory);
  if (!rebound) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(rebound.error());
  }
  return clonecore::Result<diskmodel::DiskInfo>::success(
      std::move(rebound.value().observed));
}

const diskmodel::PartitionInfo* find_created_partition(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::DiskInfo& disk) noexcept {
  const auto found = std::find_if(
      disk.partitions.begin(),
      disk.partitions.end(),
      [&](const auto& partition) {
        return partition.offset_bytes ==
                reviewed.system_partition_offset_bytes() &&
            partition.size_bytes == reviewed.system_partition_size_bytes();
      });
  return found == disk.partitions.end() ? nullptr : &*found;
}

clonecore::Status validate_created_vds_partition_exact(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::PartitionInfo& expected,
    const VdsDiskBinding& binding) {
  VDS_PARTITION_PROP properties{};
  const HRESULT result = binding.advanced->GetPartitionProperties(
      reviewed.system_partition_offset_bytes(), &properties);
  if (result != S_OK) {
    return status_failure(
        clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"起動修復システム領域作成のrollback VDS partition照合",
        L"exact created partitionをVDS objectで再取得できません");
  }
  const bool geometry_matches =
      properties.ulPartitionNumber == expected.number &&
      properties.ullOffset == reviewed.system_partition_offset_bytes() &&
      properties.ullSize == reviewed.system_partition_size_bytes();
  const bool role_matches =
      reviewed.system_role() == BootSystemPartitionRole::efi_system
      ? properties.PartitionStyle == VDS_PST_GPT &&
          IsEqualGUID(
              properties.Gpt.partitionType, kEfiSystemPartitionType) != FALSE
      : properties.PartitionStyle == VDS_PST_MBR &&
          properties.Mbr.partitionType == 0x07U &&
          properties.Mbr.bootIndicator != FALSE;
  if (!geometry_matches || !role_matches) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"起動修復システム領域作成のrollback VDS extent拘束",
        L"VDS partition number、offset、size、type、またはActive属性がraw inventoryと一致しません");
  }
  return clonecore::success_status();
}

// Create/format can fail after VDS has committed CreatePartition but before
// the automatic repair planner can inspect a filesystem.  Cleanup is allowed
// only when two raw inventory passes and the object-bound VDS partition all
// prove the one exact reviewed addition.  An unchanged shrunken layout is a
// successful no-op cleanup; every other state remains untouched.
clonecore::Status cleanup_possible_created_partition_exact(
    const ReviewedSystemPartitionCreation& reviewed) {
  auto observed = observe_raw_disk_exact(reviewed.selected_identity());
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  if (validate_system_partition_creation_shrunken_disk(
          reviewed, observed.value())) {
    return clonecore::success_status();
  }
  const auto created_layout =
      validate_system_partition_creation_created_disk_for_rollback(
          reviewed, observed.value());
  if (!created_layout) {
    return created_layout;
  }
  const auto* created = find_created_partition(reviewed, observed.value());
  if (created == nullptr) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"起動修復システム領域作成のrollback raw partition検索",
        L"完全照合した新規system partitionを一意に取得できません");
  }

  auto session = VdsSession::open();
  if (!session) {
    return clonecore::Status::failure(session.error());
  }
  auto disk = session.value()->find_disk_exact(observed.value());
  if (!disk) {
    return clonecore::Status::failure(disk.error());
  }
  auto vds_partition = validate_created_vds_partition_exact(
      reviewed, *created, disk.value());
  if (!vds_partition) {
    return vds_partition;
  }

  auto final_observed = observe_raw_disk_exact(reviewed.selected_identity());
  if (!final_observed) {
    return clonecore::Status::failure(final_observed.error());
  }
  const auto final_layout =
      validate_system_partition_creation_created_disk_for_rollback(
          reviewed, final_observed.value());
  if (!final_layout) {
    return final_layout;
  }
  const auto* final_created = find_created_partition(
      reviewed, final_observed.value());
  if (final_created == nullptr) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"起動修復システム領域作成のrollback最終raw partition検索",
        L"mutation直前に新規system partitionを再取得できません");
  }
  const auto final_vds_binding = revalidate_vds_disk_binding_exact(
      disk.value(), final_observed.value());
  if (!final_vds_binding) {
    return final_vds_binding;
  }
  vds_partition = validate_created_vds_partition_exact(
      reviewed, *final_created, disk.value());
  if (!vds_partition) {
    return vds_partition;
  }

  const HRESULT deleted = disk.value().advanced->DeletePartition(
      reviewed.system_partition_offset_bytes(), FALSE, TRUE);
  if (deleted != S_OK) {
    return status_failure(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(deleted),
        L"起動修復システム領域作成の失敗後exact cleanup",
        L"raw inventoryとVDSで完全照合した新規system partitionを削除できません");
  }
  const auto refreshed = session.value()->refresh();
  if (!refreshed) {
    return refreshed;
  }
  auto cleaned = observe_raw_disk_exact(reviewed.selected_identity());
  if (!cleaned) {
    return clonecore::Status::failure(cleaned.error());
  }
  return validate_system_partition_creation_shrunken_disk(
      reviewed, cleaned.value());
}

clonecore::Status preserve_primary_after_cleanup(
    clonecore::Status primary,
    const ReviewedSystemPartitionCreation& reviewed) {
  const auto cleaned = cleanup_possible_created_partition_exact(reviewed);
  return cleaned ? std::move(primary) : cleaned;
}

class WindowsSystemPartitionCreationPlatform final
    : public ISystemPartitionCreationPlatform {
 public:
  clonecore::Result<SystemPartitionCreationObservation> observe_read_only(
      const clonecore::StableDiskIdentity& expected_disk,
      const std::uint32_t windows_partition_number) override {
    auto plan = fresh_plan(expected_disk);
    if (!plan) {
      return clonecore::Result<SystemPartitionCreationObservation>::failure(
          plan.error());
    }
    const auto* windows = find_windows(plan.value(), windows_partition_number);
    if (windows == nullptr || windows->volume.volume_name.empty()) {
      return failure<SystemPartitionCreationObservation>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"起動修復システム領域作成のWindows volume拘束",
          L"選択したWindows partitionとVolume GUIDを一意に確認できません");
    }
    auto session = VdsSession::open();
    if (!session) {
      return clonecore::Result<SystemPartitionCreationObservation>::failure(
          session.error());
    }
    auto volume = session.value()->find_volume_exact(
        windows->volume.volume_name);
    if (!volume) {
      return clonecore::Result<SystemPartitionCreationObservation>::failure(
          volume.error());
    }
    VDS_VOLUME_PROP properties{};
    const HRESULT property_result = volume.value()->GetProperties(&properties);
    CoTaskMemFree(properties.pwszName);
    if (property_result != S_OK) {
      return failure<SystemPartitionCreationObservation>(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(property_result),
          L"起動修復システム領域作成のVDS volume属性",
          L"Windows volumeのVDS属性を取得できません");
    }
    ComPtr<IVdsVolumeShrink> shrink;
    const HRESULT interface_result = volume.value()->QueryInterface(
        IID_PPV_ARGS(shrink.put()));
    ULONGLONG reclaimable = 0U;
    const HRESULT reclaim_result =
        interface_result == S_OK && shrink
        ? shrink->QueryMaxReclaimableBytes(&reclaimable)
        : interface_result;
    const bool exact_size = properties.ullSize ==
        windows->volume.location.extent_length;
    SystemPartitionCreationObservation observation{
        .plan = plan.take_value(),
        .max_reclaimable_bytes =
            reclaim_result == S_OK ? reclaimable : 0U,
        .exact_windows_volume_found = exact_size,
        .simple_ntfs_volume = properties.type == VDS_VT_SIMPLE &&
            text_equal(windows->volume.location.file_system, L"NTFS") &&
            properties.RecommendedFileSystemType == VDS_FST_NTFS,
        .volume_online = properties.status == VDS_VS_ONLINE,
        .volume_transition_stable =
            properties.TransitionState == VDS_TS_STABLE,
        .volume_health_acceptable = properties.health == VDS_H_HEALTHY,
        .forbidden_volume_role_or_encryption =
            (properties.ulFlags & kForbiddenWindowsVolumeFlags) != 0U,
    };
    if (reclaim_result != S_OK) {
      return failure<SystemPartitionCreationObservation>(
          clonecore::ErrorCode::query_failed,
          static_cast<DWORD>(reclaim_result),
          L"起動修復システム領域作成のVDS縮小可能量",
          L"Microsoft VDSからNTFSの最大縮小可能量を取得できません");
    }
    return clonecore::Result<SystemPartitionCreationObservation>::success(
        std::move(observation));
  }

  clonecore::Status shrink_windows_exact(
      const ReviewedSystemPartitionCreation& reviewed) override {
    auto fresh = observe_read_only(
        reviewed.selected_identity(), reviewed.windows_partition().number);
    if (!fresh) {
      return clonecore::Status::failure(fresh.error());
    }
    const auto rebound = revalidate_system_partition_creation_review(
        reviewed, fresh.value());
    if (!rebound) {
      return rebound;
    }
    auto session = VdsSession::open();
    if (!session) {
      return clonecore::Status::failure(session.error());
    }
    auto volume = session.value()->find_volume_exact(
        reviewed.windows_volume_name());
    if (!volume) {
      return clonecore::Status::failure(volume.error());
    }
    ComPtr<IVdsVolumeShrink> shrink;
    HRESULT result = volume.value()->QueryInterface(
        IID_PPV_ARGS(shrink.put()));
    if (result != S_OK || !shrink) {
      return status_failure(
          clonecore::ErrorCode::unsupported_platform,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のVDS shrink取得",
          L"exact Windows NTFS volumeにVDS shrink interfaceがありません");
    }
    // A second complete planner pass after obtaining the COM object closes
    // the disk-number churn window before the object-bound mutation.
    auto final_observation = observe_read_only(
        reviewed.selected_identity(), reviewed.windows_partition().number);
    if (!final_observation) {
      return clonecore::Status::failure(final_observation.error());
    }
    const auto final_rebound = revalidate_system_partition_creation_review(
        reviewed, final_observation.value());
    if (!final_rebound) {
      return final_rebound;
    }
    const auto final_volume_binding = revalidate_vds_windows_volume_exact(
        *volume.value().get(),
        reviewed.windows_partition().size_bytes,
        L"起動修復システム領域作成のVDS shrink直前Volume GUID拘束");
    if (!final_volume_binding) {
      return final_volume_binding;
    }
    ComPtr<IVdsAsync> asynchronous;
    result = shrink->Shrink(
        reviewed.reclaim_bytes(),
        reviewed.reclaim_bytes(),
        asynchronous.put());
    if (result != S_OK || !asynchronous) {
      return status_failure(
          clonecore::ErrorCode::io_failed,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のVDS shrink開始",
          L"Microsoft VDS exact NTFS shrinkを開始できません");
    }
    VDS_ASYNC_OUTPUT output{};
    const auto waited = wait_async(
        *asynchronous.get(),
        VDS_ASYNCOUT_SHRINKVOLUME,
        output,
        L"起動修復システム領域作成のVDS shrink完了");
    if (!waited) {
      return waited;
    }
    if (output.sv.ullReclaimedBytes != reviewed.reclaim_bytes()) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"起動修復システム領域作成のVDS shrink寸法",
          L"VDSが返した実縮小量がレビュー済みexact寸法と一致しません");
    }
    return clonecore::success_status();
  }

  clonecore::Status create_and_format_system_exact(
      const ReviewedSystemPartitionCreation& reviewed) override {
    auto plan = fresh_plan(reviewed.selected_identity());
    if (!plan) {
      return clonecore::Status::failure(plan.error());
    }
    const auto binding = validate_shrunken_binding(reviewed, plan.value());
    if (!binding) {
      return binding;
    }
    auto session = VdsSession::open();
    if (!session) {
      return clonecore::Status::failure(session.error());
    }
    auto disk = session.value()->find_disk_exact(plan.value().selected_disk);
    if (!disk) {
      return clonecore::Status::failure(disk.error());
    }
    auto final_plan = fresh_plan(reviewed.selected_identity());
    if (!final_plan) {
      return clonecore::Status::failure(final_plan.error());
    }
    const auto final_binding = validate_shrunken_binding(
        reviewed, final_plan.value());
    if (!final_binding) {
      return final_binding;
    }
    const auto final_vds_binding = revalidate_vds_disk_binding_exact(
        disk.value(), final_plan.value().selected_disk);
    if (!final_vds_binding) {
      return final_vds_binding;
    }
    CREATE_PARTITION_PARAMETERS parameters{};
    if (reviewed.system_role() == BootSystemPartitionRole::efi_system) {
      parameters.style = VDS_PST_GPT;
      parameters.GptPartInfo.partitionType = kEfiSystemPartitionType;
      if (CoCreateGuid(&parameters.GptPartInfo.partitionId) != S_OK) {
        return status_failure(
            clonecore::ErrorCode::io_failed,
            ERROR_INVALID_DATA,
            L"起動修復ESP partition GUID生成",
            L"新しいESPの一意partition GUIDを生成できません");
      }
      parameters.GptPartInfo.attributes = 0U;
      constexpr wchar_t kName[] = L"SYSTEM";
      static_assert(std::size(kName) <= std::size(parameters.GptPartInfo.name));
      std::copy(std::begin(kName), std::end(kName), parameters.GptPartInfo.name);
    } else {
      parameters.style = VDS_PST_MBR;
      parameters.MbrPartInfo.partitionType = 0x07U;
      parameters.MbrPartInfo.bootIndicator = TRUE;
    }
    ComPtr<IVdsAsync> asynchronous;
    HRESULT result = disk.value().advanced->CreatePartition(
        reviewed.system_partition_offset_bytes(),
        reviewed.system_partition_size_bytes(),
        &parameters,
        asynchronous.put());
    if (result != S_OK || !asynchronous) {
      return preserve_primary_after_cleanup(
          status_failure(
              clonecore::ErrorCode::io_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域のVDS作成開始",
              L"reviewed free extentへexact system partitionを作成できません"),
          reviewed);
    }
    VDS_ASYNC_OUTPUT output{};
    auto waited = wait_async(
        *asynchronous.get(),
        VDS_ASYNCOUT_CREATEPARTITION,
        output,
        L"起動修復システム領域のVDS作成完了");
    if (!waited || output.cp.ullOffset !=
                       reviewed.system_partition_offset_bytes()) {
      auto primary = waited
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"起動修復システム領域のVDS作成offset",
                L"作成されたpartition offsetがレビュー済み値と一致しません")
          : waited;
      return preserve_primary_after_cleanup(std::move(primary), reviewed);
    }
    auto refreshed = session.value()->refresh();
    if (!refreshed) {
      return preserve_primary_after_cleanup(
          std::move(refreshed), reviewed);
    }
    ComPtr<IUnknown> volume_unknown;
    result = session.value()->service().GetObject(
        output.cp.volumeId, VDS_OT_VOLUME, volume_unknown.put());
    ComPtr<IVdsVolumeMF2> formatter;
    if (result == S_OK && volume_unknown) {
      result = volume_unknown->QueryInterface(IID_PPV_ARGS(formatter.put()));
    }
    if (result != S_OK || !formatter) {
      return preserve_primary_after_cleanup(
          status_failure(
              clonecore::ErrorCode::unsupported_platform,
              static_cast<DWORD>(result),
              L"起動修復システム領域のVDS formatter取得",
              L"新しいpartitionをexact volume formatterへ拘束できません"),
          reviewed);
    }
    std::wstring file_system =
        reviewed.system_role() == BootSystemPartitionRole::efi_system
        ? L"FAT32"
        : L"NTFS";
    std::wstring label = L"SYSTEM";
    asynchronous = ComPtr<IVdsAsync>();
    result = formatter->FormatEx(
        file_system.data(),
        0U,
        0U,
        label.data(),
        FALSE,
        TRUE,
        FALSE,
        asynchronous.put());
    if (result != S_OK || !asynchronous) {
      return preserve_primary_after_cleanup(
          status_failure(
              clonecore::ErrorCode::io_failed,
              static_cast<DWORD>(result),
              L"起動修復システム領域のVDS format開始",
              L"新しいsystem partitionのMicrosoft quick formatを開始できません"),
          reviewed);
    }
    output = {};
    waited = wait_async(
        *asynchronous.get(),
        VDS_ASYNCOUT_FORMAT,
        output,
        L"起動修復システム領域のVDS format完了");
    if (!waited) {
      return preserve_primary_after_cleanup(std::move(waited), reviewed);
    }
    return session.value()->refresh();
  }

  clonecore::Status delete_created_system_exact(
      const ReviewedSystemPartitionCreation& reviewed) override {
    auto plan = fresh_plan(reviewed.selected_identity());
    if (!plan) {
      return clonecore::Status::failure(plan.error());
    }
    const auto complete = validate_completed_binding(reviewed, plan.value());
    if (!complete) {
      return complete;
    }
    auto session = VdsSession::open();
    if (!session) {
      return clonecore::Status::failure(session.error());
    }
    auto disk = session.value()->find_disk_exact(plan.value().selected_disk);
    if (!disk) {
      return clonecore::Status::failure(disk.error());
    }
    auto final_plan = fresh_plan(reviewed.selected_identity());
    if (!final_plan) {
      return clonecore::Status::failure(final_plan.error());
    }
    const auto final_complete = validate_completed_binding(
        reviewed, final_plan.value());
    if (!final_complete) {
      return final_complete;
    }
    const auto final_vds_binding = revalidate_vds_disk_binding_exact(
        disk.value(), final_plan.value().selected_disk);
    if (!final_vds_binding) {
      return final_vds_binding;
    }
    const HRESULT result = disk.value().advanced->DeletePartition(
        reviewed.system_partition_offset_bytes(), FALSE, TRUE);
    if (result != S_OK) {
      return status_failure(
          clonecore::ErrorCode::io_failed,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のrollback削除",
          L"exact created system partitionを削除できません");
    }
    return session.value()->refresh();
  }

  clonecore::Status extend_windows_exact(
      const ReviewedSystemPartitionCreation& reviewed) override {
    auto plan = fresh_plan(reviewed.selected_identity());
    if (!plan) {
      return clonecore::Status::failure(plan.error());
    }
    const auto binding = validate_shrunken_binding(reviewed, plan.value());
    if (!binding) {
      return binding;
    }
    auto session = VdsSession::open();
    if (!session) {
      return clonecore::Status::failure(session.error());
    }
    auto disk = session.value()->find_disk_exact(plan.value().selected_disk);
    auto volume = session.value()->find_volume_exact(
        reviewed.windows_volume_name());
    if (!disk) {
      return clonecore::Status::failure(disk.error());
    }
    if (!volume) {
      return clonecore::Status::failure(volume.error());
    }
    auto final_plan = fresh_plan(reviewed.selected_identity());
    if (!final_plan) {
      return clonecore::Status::failure(final_plan.error());
    }
    const auto final_binding = validate_shrunken_binding(
        reviewed, final_plan.value());
    if (!final_binding) {
      return final_binding;
    }
    const auto final_vds_binding = revalidate_vds_disk_binding_exact(
        disk.value(), final_plan.value().selected_disk);
    if (!final_vds_binding) {
      return final_vds_binding;
    }
    const auto final_volume_binding = revalidate_vds_windows_volume_exact(
        *volume.value().get(),
        reviewed.shrunken_windows_size_bytes(),
        L"起動修復システム領域作成のrollback extend直前Volume GUID拘束");
    if (!final_volume_binding) {
      return final_volume_binding;
    }
    VDS_INPUT_DISK input{
        .diskId = disk.value().id,
        .ullSize = reviewed.reclaim_bytes(),
        .plexId = GUID_NULL,
        .memberIdx = 0U,
    };
    ComPtr<IVdsAsync> asynchronous;
    const HRESULT result = volume.value()->Extend(
        &input, 1, asynchronous.put());
    if (result != S_OK || !asynchronous) {
      return status_failure(
          clonecore::ErrorCode::io_failed,
          static_cast<DWORD>(result),
          L"起動修復システム領域作成のrollback拡張",
          L"Windows NTFS extentを元のexact寸法へ拡張できません");
    }
    VDS_ASYNC_OUTPUT output{};
    return wait_async(
        *asynchronous.get(),
        VDS_ASYNCOUT_EXTENDVOLUME,
        output,
        L"起動修復システム領域作成のrollback拡張完了");
  }
};

}  // namespace

std::unique_ptr<ISystemPartitionCreationPlatform>
make_windows_system_partition_creation_platform() {
  return std::make_unique<WindowsSystemPartitionCreationPlatform>();
}

}  // namespace ytec::bootrepair

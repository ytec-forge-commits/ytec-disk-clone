#include "ytec/vssrequester/windows_diff_area_observer.h"
#include "ytec/vssrequester/windows_backend.h"

#include <Windows.h>
#include <objbase.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <vsserror.h>
#include <vsmgmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string_view>
#include <utility>

namespace ytec::vssrequester {
namespace {

clonecore::Error observer_error(
    const clonecore::ErrorCode code,
    const HRESULT native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = static_cast<DWORD>(native_code),
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
             [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
             });
}

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

bool is_guid_string(const std::wstring_view value) noexcept {
  if (value.size() != 38U || value.front() != L'{' ||
      value.back() != L'}') {
    return false;
  }
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    const bool hyphen =
        index == 9U || index == 14U || index == 19U || index == 24U;
    if ((hyphen && value[index] != L'-') ||
        (!hyphen && !is_hex(value[index]))) {
      return false;
    }
  }
  return true;
}

bool is_volume_guid_path(const std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (path.size() != 49U || !path.starts_with(prefix) ||
      path[47] != L'}' || path[48] != L'\\') {
    return false;
  }
  for (std::size_t index = prefix.size(); index < 47U; ++index) {
    const std::size_t guid_index = index - prefix.size();
    const bool hyphen = guid_index == 8U || guid_index == 13U ||
                        guid_index == 18U || guid_index == 23U;
    if ((hyphen && path[index] != L'-') ||
        (!hyphen && !is_hex(path[index]))) {
      return false;
    }
  }
  return true;
}

bool is_snapshot_device_path(const std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (!path.starts_with(prefix) || path.size() <= prefix.size()) {
    return false;
  }
  std::wstring_view suffix = path.substr(prefix.size());
  if (suffix.ends_with(L'\\')) {
    suffix.remove_suffix(1U);
  }
  return !suffix.empty() &&
         std::all_of(
             suffix.begin(), suffix.end(), [](const wchar_t value) {
               return value >= L'0' && value <= L'9';
             });
}

bool guid_is_null(const GUID& value) noexcept {
  return IsEqualGUID(value, GUID_NULL) != FALSE;
}

clonecore::Result<GUID> parse_guid(
    const std::wstring& value,
    std::wstring operation) {
  GUID parsed = GUID_NULL;
  const HRESULT result = CLSIDFromString(value.c_str(), &parsed);
  if (FAILED(result) || guid_is_null(parsed)) {
    return clonecore::Result<GUID>::failure(observer_error(
        clonecore::ErrorCode::invalid_argument,
        FAILED(result) ? result : E_INVALIDARG,
        std::move(operation),
        L"固定済みVSS GUIDを安全に解析できません"));
  }
  return clonecore::Result<GUID>::success(parsed);
}

template <typename Interface>
class ComPtr final {
 public:
  ComPtr() = default;
  ~ComPtr() { reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&&) = delete;
  ComPtr& operator=(ComPtr&&) = delete;

  [[nodiscard]] Interface* get() const noexcept { return value_; }
  [[nodiscard]] Interface* operator->() const noexcept { return value_; }

  [[nodiscard]] Interface** put() noexcept {
    reset();
    return &value_;
  }

  void attach(Interface* const value) noexcept {
    reset();
    value_ = value;
  }

  void reset() noexcept {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  Interface* value_{};
};

class ComApartment final {
 public:
  [[nodiscard]] clonecore::Status initialize() {
    const HRESULT result =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (result == S_OK || result == S_FALSE) {
      must_uninitialize_ = true;
      return clonecore::success_status();
    }
    if (result == RPC_E_CHANGED_MODE) {
      // The owning application may already have initialized this worker as an
      // STA. COM remains usable; this call has no matching CoUninitialize.
      return clonecore::success_status();
    }
    return clonecore::Status::failure(observer_error(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS差分領域COM初期化",
        L"COM apartmentを初期化できませんでした"));
  }

  ~ComApartment() {
    if (must_uninitialize_) {
      CoUninitialize();
    }
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  ComApartment() = default;

 private:
  bool must_uninitialize_{};
};

class SnapshotObject final {
 public:
  SnapshotObject() = default;
  ~SnapshotObject() { reset(); }

  SnapshotObject(const SnapshotObject&) = delete;
  SnapshotObject& operator=(const SnapshotObject&) = delete;

  [[nodiscard]] VSS_OBJECT_PROP* put() noexcept {
    reset();
    return &value_;
  }
  [[nodiscard]] const VSS_OBJECT_PROP& get() const noexcept {
    return value_;
  }

 private:
  void reset() noexcept {
    if (value_.Type == VSS_OBJECT_SNAPSHOT) {
      VssFreeSnapshotProperties(&value_.Obj.Snap);
    } else if (value_.Type == VSS_OBJECT_PROVIDER) {
      CoTaskMemFree(value_.Obj.Prov.m_pwszProviderName);
      CoTaskMemFree(value_.Obj.Prov.m_pwszProviderVersion);
    }
    value_ = {};
  }

  VSS_OBJECT_PROP value_{};
};

void free_management_object(VSS_MGMT_OBJECT_PROP& value) noexcept {
  switch (value.Type) {
    case VSS_MGMT_OBJECT_VOLUME:
      CoTaskMemFree(value.Obj.Vol.m_pwszVolumeName);
      CoTaskMemFree(value.Obj.Vol.m_pwszVolumeDisplayName);
      break;
    case VSS_MGMT_OBJECT_DIFF_VOLUME:
      CoTaskMemFree(value.Obj.DiffVol.m_pwszVolumeName);
      CoTaskMemFree(value.Obj.DiffVol.m_pwszVolumeDisplayName);
      break;
    case VSS_MGMT_OBJECT_DIFF_AREA:
      CoTaskMemFree(value.Obj.DiffArea.m_pwszVolumeName);
      CoTaskMemFree(value.Obj.DiffArea.m_pwszDiffAreaVolumeName);
      break;
    case VSS_MGMT_OBJECT_UNKNOWN:
    default:
      break;
  }
  value = {};
}

class ManagementObject final {
 public:
  ManagementObject() = default;
  ~ManagementObject() { free_management_object(value_); }

  ManagementObject(const ManagementObject&) = delete;
  ManagementObject& operator=(const ManagementObject&) = delete;

  [[nodiscard]] VSS_MGMT_OBJECT_PROP* put() noexcept {
    free_management_object(value_);
    return &value_;
  }
  [[nodiscard]] const VSS_MGMT_OBJECT_PROP& get() const noexcept {
    return value_;
  }

 private:
  VSS_MGMT_OBJECT_PROP value_{};
};

struct SnapshotIdentity final {
  GUID snapshot_id{GUID_NULL};
  GUID snapshot_set_id{GUID_NULL};
  GUID provider_id{GUID_NULL};
  std::wstring original_volume_guid_path;
  std::wstring snapshot_device_path;
  LONG attributes{};
  VSS_TIMESTAMP creation_timestamp{};
  VSS_SNAPSHOT_STATE state{VSS_SS_UNKNOWN};
};

clonecore::Result<SnapshotIdentity> query_and_validate_snapshot(
    IVssSnapshotMgmt& management,
    const GUID& expected_snapshot_id,
    const GUID& expected_snapshot_set_id,
    const VssDiffAreaSnapshotBinding& expected) {
  auto expected_provider = parse_guid(
      expected.provider_id,
      L"VSS差分領域Provider GUID解析");
  if (!expected_provider) {
    return clonecore::Result<SnapshotIdentity>::failure(
        expected_provider.error());
  }
  ComPtr<IVssEnumObject> enumeration;
  HRESULT result = management.QuerySnapshotsByVolume(
      const_cast<VSS_PWSZ>(expected.original_volume_guid_path.c_str()),
      GUID_NULL,
      enumeration.put());
  if (FAILED(result) || enumeration.get() == nullptr) {
    return clonecore::Result<SnapshotIdentity>::failure(observer_error(
        result == VSS_E_OBJECT_NOT_FOUND
            ? clonecore::ErrorCode::identity_mismatch
            : clonecore::ErrorCode::query_failed,
        FAILED(result) ? result : E_POINTER,
        L"VSS差分領域Snapshot属性取得",
        result == VSS_E_OBJECT_NOT_FOUND
            ? L"監視対象Snapshotが存在しません"
            : L"QuerySnapshotsByVolumeに失敗しました"));
  }

  std::optional<SnapshotIdentity> identity;
  std::size_t enumerated_count = 0U;
  for (;;) {
    SnapshotObject object;
    ULONG fetched = 0U;
    result = enumeration->Next(1U, object.put(), &fetched);
    if (result == S_FALSE && fetched == 0U) {
      break;
    }
    if (FAILED(result) || fetched != 1U || result != S_OK ||
        object.get().Type != VSS_OBJECT_SNAPSHOT) {
      return clonecore::Result<SnapshotIdentity>::failure(observer_error(
          clonecore::ErrorCode::query_failed,
          FAILED(result) ? result : E_UNEXPECTED,
          L"VSS差分領域Snapshot列挙",
          L"Snapshot列挙が正規の単一Snapshot要素を返しませんでした"));
    }
    ++enumerated_count;
    if (enumerated_count > 4'096U) {
      return clonecore::Result<SnapshotIdentity>::failure(observer_error(
          clonecore::ErrorCode::verification_failed,
          VSS_E_BAD_STATE,
          L"VSS差分領域Snapshot列挙上限",
          L"Source VolumeのSnapshot件数が安全上限を超えました"));
    }
    const auto& candidate = object.get().Obj.Snap;
    if (!IsEqualGUID(candidate.m_SnapshotId, expected_snapshot_id)) {
      continue;
    }
    if (identity.has_value()) {
      return clonecore::Result<SnapshotIdentity>::failure(observer_error(
          clonecore::ErrorCode::identity_mismatch,
          VSS_E_BAD_STATE,
          L"VSS差分領域Snapshot一意性検証",
          L"同じSnapshot IDが一意に列挙されませんでした"));
    }

    const std::wstring original =
        candidate.m_pwszOriginalVolumeName == nullptr
        ? L""
        : std::wstring(candidate.m_pwszOriginalVolumeName);
    const std::wstring device =
        candidate.m_pwszSnapshotDeviceObject == nullptr
        ? L""
        : std::wstring(candidate.m_pwszSnapshotDeviceObject);
    if (!IsEqualGUID(candidate.m_SnapshotSetId, expected_snapshot_set_id) ||
        guid_is_null(candidate.m_ProviderId) ||
        !IsEqualGUID(candidate.m_ProviderId, expected_provider.value()) ||
        candidate.m_tsCreationTimestamp != expected.creation_timestamp ||
        !equals_case_insensitive(
            original, expected.original_volume_guid_path) ||
        !equals_case_insensitive(device, expected.snapshot_device_path) ||
        !is_volume_guid_path(original) ||
        !is_snapshot_device_path(device) ||
        candidate.m_eStatus != VSS_SS_CREATED) {
      return clonecore::Result<SnapshotIdentity>::failure(observer_error(
          clonecore::ErrorCode::identity_mismatch,
          VSS_E_OBJECT_NOT_FOUND,
          L"VSS差分領域Snapshot Identity検証",
          L"Snapshot属性が固定済みSet、Source Volume、デバイス、または作成済み状態と一致しません"));
    }
    identity = SnapshotIdentity{
        .snapshot_id = candidate.m_SnapshotId,
        .snapshot_set_id = candidate.m_SnapshotSetId,
        .provider_id = candidate.m_ProviderId,
        .original_volume_guid_path = original,
        .snapshot_device_path = device,
        .attributes = candidate.m_lSnapshotAttributes,
        .creation_timestamp = candidate.m_tsCreationTimestamp,
        .state = candidate.m_eStatus,
    };
  }

  if (!identity.has_value()) {
    return clonecore::Result<SnapshotIdentity>::failure(observer_error(
        clonecore::ErrorCode::identity_mismatch,
        VSS_E_OBJECT_NOT_FOUND,
        L"VSS差分領域Snapshot存在検証",
        L"固定済みSource Volumeに監視対象Snapshotが存在しません"));
  }
  return clonecore::Result<SnapshotIdentity>::success(
      std::move(*identity));
}

bool same_snapshot_identity(
    const SnapshotIdentity& left,
    const SnapshotIdentity& right) {
  return IsEqualGUID(left.snapshot_id, right.snapshot_id) != FALSE &&
         IsEqualGUID(left.snapshot_set_id, right.snapshot_set_id) != FALSE &&
         IsEqualGUID(left.provider_id, right.provider_id) != FALSE &&
         equals_case_insensitive(
             left.original_volume_guid_path,
             right.original_volume_guid_path) &&
         equals_case_insensitive(
             left.snapshot_device_path, right.snapshot_device_path) &&
         left.attributes == right.attributes &&
         left.creation_timestamp == right.creation_timestamp &&
         left.state == right.state;
}

clonecore::Result<std::wstring> probe_source_identity(
    const WindowsVssSourceIdentityProbe& probe,
    const VssDiffAreaSnapshotBinding& binding) {
  if (!probe) {
    return clonecore::Result<std::wstring>::failure(observer_error(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS差分領域Source再識別境界",
        L"read-only source identity probeが設定されていません"));
  }

  try {
    auto observed = probe(binding);
    if (!observed) {
      return observed;
    }
    if (observed.value().empty() ||
        observed.value() != binding.expected_source_identity_token) {
      return clonecore::Result<std::wstring>::failure(observer_error(
          clonecore::ErrorCode::identity_mismatch,
          VSS_E_OBJECT_NOT_FOUND,
          L"VSS差分領域Source identity再検証",
          L"SnapshotのSource Volumeを支えるsource identityが固定値から変化しました"));
    }
    return observed;
  } catch (...) {
    return clonecore::Result<std::wstring>::failure(observer_error(
        clonecore::ErrorCode::internal_error,
        E_UNEXPECTED,
        L"VSS差分領域Source再識別",
        L"read-only source identity probeが例外を送出しました"));
  }
}

clonecore::Result<VssDiffAreaObservation>
observe_backing_volume_space_read_only(
    VssDiffAreaObservation observation) {
  if (!is_volume_guid_path(observation.diff_area_volume_guid_path)) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS差分領域backing Volume境界",
        L"VSS associationが正規のVolume GUIDを返していません"));
  }

  const auto resolve_canonical = [&](std::wstring operation)
      -> clonecore::Result<std::wstring> {
    std::array<wchar_t, MAX_PATH + 1U> buffer{};
    if (!GetVolumeNameForVolumeMountPointW(
            observation.diff_area_volume_guid_path.c_str(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()))) {
      return clonecore::Result<std::wstring>::failure(observer_error(
          clonecore::ErrorCode::query_failed,
          HRESULT_FROM_WIN32(GetLastError()),
          std::move(operation),
          L"差分領域を支えるcanonical Volume GUIDを取得できません"));
    }
    std::wstring canonical(buffer.data());
    if (!is_volume_guid_path(canonical) ||
        !equals_case_insensitive(
            canonical, observation.diff_area_volume_guid_path)) {
      return clonecore::Result<std::wstring>::failure(observer_error(
          clonecore::ErrorCode::identity_mismatch,
          HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
          std::move(operation),
          L"VSS associationとcanonical backing Volumeが一意に一致しません"));
    }
    return clonecore::Result<std::wstring>::success(std::move(canonical));
  };

  auto canonical_before = resolve_canonical(
      L"VSS差分領域backing Volume前方再識別");
  if (!canonical_before) {
    return clonecore::Result<VssDiffAreaObservation>::failure(
        canonical_before.error());
  }

  DWORD serial_before = 0U;
  if (!GetVolumeInformationW(
          canonical_before.value().c_str(),
          nullptr,
          0U,
          &serial_before,
          nullptr,
          nullptr,
          nullptr,
          0U)) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::query_failed,
        HRESULT_FROM_WIN32(GetLastError()),
        L"VSS差分領域backing Volume serial取得",
        L"差分領域を支えるVolumeのread-only serialを取得できません"));
  }

  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free{};
  if (!GetDiskFreeSpaceExW(
          canonical_before.value().c_str(), &available, &total, &free)) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::query_failed,
        HRESULT_FROM_WIN32(GetLastError()),
        L"VSS差分領域backing Volume空き容量取得",
        L"差分領域を支えるVolumeの空き容量をread-onlyで取得できません"));
  }

  auto canonical_after = resolve_canonical(
      L"VSS差分領域backing Volume後方再識別");
  if (!canonical_after) {
    return clonecore::Result<VssDiffAreaObservation>::failure(
        canonical_after.error());
  }
  DWORD serial_after = 0U;
  if (!GetVolumeInformationW(
          canonical_after.value().c_str(),
          nullptr,
          0U,
          &serial_after,
          nullptr,
          nullptr,
          nullptr,
          0U)) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::query_failed,
        HRESULT_FROM_WIN32(GetLastError()),
        L"VSS差分領域backing Volume serial再取得",
        L"空き容量観測後のbacking Volumeを再識別できません"));
  }
  if (!equals_case_insensitive(
          canonical_before.value(), canonical_after.value()) ||
      serial_before != serial_after) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::identity_mismatch,
        HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
        L"VSS差分領域backing Volume差替え検証",
        L"空き容量観測中にbacking Volume identityが変化しました"));
  }
  if (total.QuadPart == 0U || free.QuadPart > total.QuadPart ||
      available.QuadPart > total.QuadPart) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::invalid_data,
        HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW),
        L"VSS差分領域backing Volume容量検証",
        L"backing Volumeの有限なtotal/free/available容量関係が不正です"));
  }

  observation.backing_volume_guid_path = canonical_after.take_value();
  observation.backing_volume_serial_number = serial_after;
  observation.backing_volume_total_bytes = total.QuadPart;
  observation.backing_volume_free_bytes = free.QuadPart;
  observation.backing_volume_available_bytes = available.QuadPart;
  return clonecore::Result<VssDiffAreaObservation>::success(
      std::move(observation));
}

clonecore::Result<VssDiffAreaObservation> validate_backing_volume_probe(
    const VssDiffAreaObservation& expected,
    clonecore::Result<VssDiffAreaObservation> probed) {
  if (!probed) {
    return probed;
  }
  const auto& value = probed.value();
  if (!equals_case_insensitive(value.snapshot_set_id, expected.snapshot_set_id) ||
      !equals_case_insensitive(value.snapshot_id, expected.snapshot_id) ||
      !equals_case_insensitive(
          value.original_volume_guid_path,
          expected.original_volume_guid_path) ||
      !equals_case_insensitive(
          value.snapshot_device_path, expected.snapshot_device_path) ||
      !equals_case_insensitive(value.provider_id, expected.provider_id) ||
      value.creation_timestamp != expected.creation_timestamp ||
      value.observed_source_identity_token !=
          expected.observed_source_identity_token ||
      !equals_case_insensitive(
          value.diff_area_volume_guid_path,
          expected.diff_area_volume_guid_path) ||
      !is_volume_guid_path(value.backing_volume_guid_path) ||
      !equals_case_insensitive(
          value.backing_volume_guid_path,
          expected.diff_area_volume_guid_path) ||
      value.maximum_kind != expected.maximum_kind ||
      value.maximum_bytes != expected.maximum_bytes ||
      value.allocated_bytes != expected.allocated_bytes ||
      value.used_bytes != expected.used_bytes ||
      value.backing_volume_total_bytes == 0U ||
      value.backing_volume_free_bytes > value.backing_volume_total_bytes ||
      value.backing_volume_available_bytes >
          value.backing_volume_total_bytes) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::identity_mismatch,
        HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
        L"VSS差分領域backing Volume probe検証",
        L"backing Volume probeが固定Snapshot associationまたは有限容量と一致しません"));
  }
  return probed;
}

clonecore::Result<VssDiffAreaObservation> query_diff_area(
    IVssSnapshotMgmt& management,
    const VssDiffAreaMonitorBinding& binding,
    const VssDiffAreaSnapshotBinding& expected,
    const SnapshotIdentity& snapshot,
    const std::wstring& observed_source_identity,
    const WindowsVssBackingVolumeSpaceProbe& backing_volume_probe) {
  ComPtr<IUnknown> provider_unknown;
  HRESULT result = management.GetProviderMgmtInterface(
      snapshot.provider_id,
      __uuidof(IVssDifferentialSoftwareSnapshotMgmt),
      provider_unknown.put());
  if (FAILED(result) || provider_unknown.get() == nullptr) {
    return clonecore::Result<VssDiffAreaObservation>::failure(
        observer_error(
            clonecore::ErrorCode::unsupported_platform,
            FAILED(result) ? result : E_NOINTERFACE,
            L"VSS差分領域Provider interface取得",
            L"Snapshot providerの読取り専用差分領域interfaceを取得できません"));
  }

  ComPtr<IVssDifferentialSoftwareSnapshotMgmt> differential;
  result = provider_unknown->QueryInterface(
      IID_PPV_ARGS(differential.put()));
  if (FAILED(result) || differential.get() == nullptr) {
    return clonecore::Result<VssDiffAreaObservation>::failure(
        observer_error(
            clonecore::ErrorCode::unsupported_platform,
            FAILED(result) ? result : E_NOINTERFACE,
            L"VSS差分領域Provider interface検証",
            L"取得したprovider interfaceを差分領域observerとして検証できません"));
  }

  ComPtr<IVssEnumMgmtObject> enumeration;
  result = differential->QueryDiffAreasForSnapshot(
      snapshot.snapshot_id, enumeration.put());
  if (FAILED(result) || enumeration.get() == nullptr) {
    return clonecore::Result<VssDiffAreaObservation>::failure(
        observer_error(
            result == VSS_E_OBJECT_NOT_FOUND
                ? clonecore::ErrorCode::identity_mismatch
                : clonecore::ErrorCode::query_failed,
            FAILED(result) ? result : E_POINTER,
            L"VSS差分領域Snapshot query",
            result == VSS_E_OBJECT_NOT_FOUND
                ? L"監視対象Snapshotまたは差分領域が存在しません"
                : L"QueryDiffAreasForSnapshotに失敗しました"));
  }

  std::optional<VssDiffAreaObservation> observation;
  for (;;) {
    ManagementObject object;
    ULONG fetched = 0U;
    result = enumeration->Next(1U, object.put(), &fetched);
    if (result == S_FALSE && fetched == 0U) {
      break;
    }
    if (FAILED(result) || fetched != 1U || result != S_OK) {
      return clonecore::Result<VssDiffAreaObservation>::failure(
          observer_error(
              clonecore::ErrorCode::query_failed,
              FAILED(result) ? result : E_UNEXPECTED,
              L"VSS差分領域列挙",
              L"差分領域列挙が正規の単一要素を返しませんでした"));
    }
    if (observation.has_value() ||
        object.get().Type != VSS_MGMT_OBJECT_DIFF_AREA) {
      return clonecore::Result<VssDiffAreaObservation>::failure(
          observer_error(
              clonecore::ErrorCode::verification_failed,
              VSS_E_BAD_STATE,
              L"VSS差分領域一意性検証",
              L"1つのSnapshotに一意な差分領域associationがありません"));
    }

    const auto& value = object.get().Obj.DiffArea;
    const std::wstring original = value.m_pwszVolumeName == nullptr
        ? L""
        : std::wstring(value.m_pwszVolumeName);
    const std::wstring diff_volume =
        value.m_pwszDiffAreaVolumeName == nullptr
        ? L""
        : std::wstring(value.m_pwszDiffAreaVolumeName);
    if (!equals_case_insensitive(
            original, expected.original_volume_guid_path) ||
        !is_volume_guid_path(original) ||
        !is_volume_guid_path(diff_volume)) {
      return clonecore::Result<VssDiffAreaObservation>::failure(
          observer_error(
              clonecore::ErrorCode::identity_mismatch,
              VSS_E_OBJECT_NOT_FOUND,
              L"VSS差分領域Volume binding検証",
              L"差分領域associationが固定済みSource Volumeと一致しません"));
    }

    VssDiffAreaMaximumKind maximum_kind =
        VssDiffAreaMaximumKind::bounded;
    if (value.m_llMaximumDiffSpace == VSS_ASSOC_NO_MAX_SPACE) {
      maximum_kind = VssDiffAreaMaximumKind::unbounded;
    } else if (value.m_llMaximumDiffSpace < 0) {
      maximum_kind = VssDiffAreaMaximumKind::unknown;
    }
    if (maximum_kind != VssDiffAreaMaximumKind::bounded ||
        value.m_llMaximumDiffSpace == 0 ||
        value.m_llAllocatedDiffSpace < 0 ||
        value.m_llUsedDiffSpace < 0) {
      return clonecore::Result<VssDiffAreaObservation>::failure(
          observer_error(
              clonecore::ErrorCode::verification_failed,
              HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW),
              L"VSS差分領域有限上限検証",
              L"差分領域の有限な64bit最大容量、割当量、使用量を確認できません"));
    }

    const auto maximum =
        static_cast<std::uint64_t>(value.m_llMaximumDiffSpace);
    const auto allocated =
        static_cast<std::uint64_t>(value.m_llAllocatedDiffSpace);
    const auto used =
        static_cast<std::uint64_t>(value.m_llUsedDiffSpace);
    if (used > allocated || allocated > maximum) {
      return clonecore::Result<VssDiffAreaObservation>::failure(
          observer_error(
              clonecore::ErrorCode::invalid_data,
              HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW),
              L"VSS差分領域64bit容量関係検証",
              L"使用量、割当量、最大容量の関係が不正です"));
    }
    VssDiffAreaObservation base{
        .snapshot_set_id = binding.snapshot_set_id,
        .snapshot_id = expected.snapshot_id,
        .original_volume_guid_path = original,
        .snapshot_device_path = expected.snapshot_device_path,
        .provider_id = expected.provider_id,
        .creation_timestamp = expected.creation_timestamp,
        .observed_source_identity_token = observed_source_identity,
        .diff_area_volume_guid_path = diff_volume,
        .maximum_kind = maximum_kind,
        .maximum_bytes = maximum,
        .allocated_bytes = allocated,
        .used_bytes = used,
    };
    const VssDiffAreaObservation expected_probe = base;
    auto probed = backing_volume_probe
        ? backing_volume_probe(base)
        : observe_backing_volume_space_read_only(std::move(base));
    auto verified = validate_backing_volume_probe(
        expected_probe, std::move(probed));
    if (!verified) {
      return clonecore::Result<VssDiffAreaObservation>::failure(
          verified.error());
    }
    observation = verified.take_value();
  }

  if (!observation.has_value()) {
    return clonecore::Result<VssDiffAreaObservation>::failure(observer_error(
        clonecore::ErrorCode::identity_mismatch,
        VSS_E_OBJECT_NOT_FOUND,
        L"VSS差分領域存在検証",
        L"監視対象Snapshotに対応する差分領域がありません"));
  }
  return clonecore::Result<VssDiffAreaObservation>::success(
      std::move(*observation));
}

}  // namespace

clonecore::Result<VssDiffAreaMonitorBinding>
make_windows_vss_diff_area_monitor_binding(
    const SnapshotCopyContext& context,
    const std::wstring& expected_source_identity_token) {
  try {
    if (!is_guid_string(context.snapshot_set_id) ||
        context.mappings.empty() || context.mappings.size() > 128U ||
        expected_source_identity_token.empty() ||
        expected_source_identity_token.size() > 512U) {
      return clonecore::Result<VssDiffAreaMonitorBinding>::failure(
          observer_error(
              clonecore::ErrorCode::invalid_argument,
              E_INVALIDARG,
              L"VSS差分領域Snapshot callback binding",
              L"Snapshot Set、Snapshot件数、またはsource identity tokenが不正です"));
    }
    auto parsed_set = parse_guid(
        context.snapshot_set_id,
        L"VSS差分領域Snapshot callback Set GUID");
    if (!parsed_set) {
      return clonecore::Result<VssDiffAreaMonitorBinding>::failure(
          parsed_set.error());
    }

    VssDiffAreaMonitorBinding binding{
        .snapshot_set_id = context.snapshot_set_id,
    };
    binding.snapshots.reserve(context.mappings.size());
    for (std::size_t index = 0U; index < context.mappings.size(); ++index) {
      const auto& mapping = context.mappings[index];
      if (!is_guid_string(mapping.snapshot_id) ||
          !is_volume_guid_path(mapping.original_volume_guid_path) ||
          !is_snapshot_device_path(mapping.snapshot_device_path) ||
          !is_guid_string(mapping.provider_id) ||
          mapping.creation_timestamp == 0) {
        return clonecore::Result<VssDiffAreaMonitorBinding>::failure(
            observer_error(
                clonecore::ErrorCode::identity_mismatch,
                E_INVALIDARG,
                L"VSS差分領域Snapshot callback mapping",
                L"Snapshot、Source Volume、device、provider、またはcreation timestampが不正です"));
      }
      auto parsed_snapshot = parse_guid(
          mapping.snapshot_id,
          L"VSS差分領域Snapshot callback Snapshot GUID");
      auto parsed_provider = parse_guid(
          mapping.provider_id,
          L"VSS差分領域Snapshot callback provider GUID");
      if (!parsed_snapshot || !parsed_provider) {
        return clonecore::Result<VssDiffAreaMonitorBinding>::failure(
            parsed_snapshot ? parsed_provider.error()
                            : parsed_snapshot.error());
      }
      for (std::size_t previous = 0U; previous < index; ++previous) {
        const auto& earlier = context.mappings[previous];
        if (equals_case_insensitive(
                mapping.snapshot_id, earlier.snapshot_id) ||
            equals_case_insensitive(
                mapping.original_volume_guid_path,
                earlier.original_volume_guid_path) ||
            equals_case_insensitive(
                mapping.snapshot_device_path,
                earlier.snapshot_device_path)) {
          return clonecore::Result<VssDiffAreaMonitorBinding>::failure(
              observer_error(
                  clonecore::ErrorCode::identity_mismatch,
                  HRESULT_FROM_WIN32(ERROR_DUP_NAME),
                  L"VSS差分領域Snapshot callback mapping重複",
                  L"Snapshot、Source Volume、およびSnapshot deviceは一意である必要があります"));
        }
      }
      binding.snapshots.push_back(VssDiffAreaSnapshotBinding{
          .snapshot_id = mapping.snapshot_id,
          .original_volume_guid_path = mapping.original_volume_guid_path,
          .snapshot_device_path = mapping.snapshot_device_path,
          .provider_id = mapping.provider_id,
          .creation_timestamp = mapping.creation_timestamp,
          .expected_source_identity_token =
              expected_source_identity_token,
      });
    }
    return clonecore::Result<VssDiffAreaMonitorBinding>::success(
        std::move(binding));
  } catch (...) {
    return clonecore::Result<VssDiffAreaMonitorBinding>::failure(
        observer_error(
            clonecore::ErrorCode::internal_error,
            E_UNEXPECTED,
            L"VSS差分領域Snapshot callback binding",
            L"Snapshot callbackから不変監視bindingを作成できませんでした"));
  }
}

clonecore::Result<std::unique_ptr<VssDiffAreaOperationMonitor>>
make_windows_vss_diff_area_operation_monitor(
    const SnapshotCopyContext& context,
    WindowsVssDiffAreaOperationMonitorOptions options) {
  try {
    if (!options.probe_source_identity || !options.review_callback) {
      return clonecore::Result<
          std::unique_ptr<VssDiffAreaOperationMonitor>>::failure(
          observer_error(
              clonecore::ErrorCode::invalid_argument,
              E_INVALIDARG,
              L"VSS差分領域製品monitor依存",
              L"fresh source identity probeと利用者review callbackが必要です"));
    }
    auto binding = make_windows_vss_diff_area_monitor_binding(
        context, options.expected_source_identity_token);
    if (!binding) {
      return clonecore::Result<
          std::unique_ptr<VssDiffAreaOperationMonitor>>::failure(
          binding.error());
    }
    std::unique_ptr<IVssDiffAreaObserver> observer =
        std::make_unique<WindowsVssDiffAreaObserver>(
            WindowsVssDiffAreaObserverOptions{
                .probe_source_identity =
                    std::move(options.probe_source_identity),
                .logger = options.logger,
            });
    return VssDiffAreaOperationMonitor::create(
        options.policy,
        binding.take_value(),
        std::move(observer),
        std::move(options.review_callback));
  } catch (...) {
    return clonecore::Result<
        std::unique_ptr<VssDiffAreaOperationMonitor>>::failure(
        observer_error(
            clonecore::ErrorCode::internal_error,
            E_UNEXPECTED,
            L"VSS差分領域製品monitor生成",
            L"製品用Windows monitorを安全に作成できませんでした"));
  }
}

class WindowsVssDiffAreaObserver::Impl final {
 public:
  explicit Impl(WindowsVssDiffAreaObserverOptions supplied_options)
      : options(std::move(supplied_options)) {}

  WindowsVssDiffAreaObserverOptions options;
};

WindowsVssDiffAreaObserver::WindowsVssDiffAreaObserver(
    WindowsVssDiffAreaObserverOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

WindowsVssDiffAreaObserver::~WindowsVssDiffAreaObserver() = default;

clonecore::Result<std::vector<VssDiffAreaObservation>>
WindowsVssDiffAreaObserver::observe(
    const VssDiffAreaMonitorBinding& binding) {
  try {
    if (!impl_->options.probe_source_identity ||
        binding.snapshots.empty() || binding.snapshots.size() > 128U) {
      return clonecore::Result<
          std::vector<VssDiffAreaObservation>>::failure(observer_error(
          clonecore::ErrorCode::invalid_argument,
          E_INVALIDARG,
          L"VSS差分領域Windows observer境界",
          L"source identity probeと1件以上128件以下のSnapshot bindingが必要です"));
    }

    if (!is_guid_string(binding.snapshot_set_id)) {
      return clonecore::Result<
          std::vector<VssDiffAreaObservation>>::failure(observer_error(
          clonecore::ErrorCode::invalid_argument,
          E_INVALIDARG,
          L"VSS差分領域Snapshot Set形式検証",
          L"Snapshot Set GUIDが正規形式ではありません"));
    }
    auto snapshot_set_id = parse_guid(
        binding.snapshot_set_id,
        L"VSS差分領域Snapshot Set GUID解析");
    if (!snapshot_set_id) {
      return clonecore::Result<
          std::vector<VssDiffAreaObservation>>::failure(
          snapshot_set_id.error());
    }

    std::vector<GUID> snapshot_ids;
    snapshot_ids.reserve(binding.snapshots.size());
    for (std::size_t index = 0U; index < binding.snapshots.size(); ++index) {
      const auto& expected = binding.snapshots[index];
      if (!is_guid_string(expected.snapshot_id) ||
          !is_volume_guid_path(expected.original_volume_guid_path) ||
          !is_snapshot_device_path(expected.snapshot_device_path) ||
          !is_guid_string(expected.provider_id) ||
          expected.creation_timestamp == 0 ||
          expected.expected_source_identity_token.empty() ||
          expected.expected_source_identity_token.size() > 512U) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(observer_error(
            clonecore::ErrorCode::invalid_argument,
            E_INVALIDARG,
            L"VSS差分領域Windows binding検証",
            L"Source Volume、Snapshotデバイス、provider、creation timestamp、またはsource identity tokenが不正です"));
      }
      auto parsed = parse_guid(
          expected.snapshot_id,
          L"VSS差分領域Snapshot GUID解析");
      if (!parsed) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(parsed.error());
      }
      for (std::size_t previous = 0U; previous < index; ++previous) {
        const auto& earlier = binding.snapshots[previous];
        if (IsEqualGUID(parsed.value(), snapshot_ids[previous]) != FALSE ||
            equals_case_insensitive(
                expected.original_volume_guid_path,
                earlier.original_volume_guid_path) ||
            equals_case_insensitive(
                expected.snapshot_device_path,
                earlier.snapshot_device_path)) {
          return clonecore::Result<
              std::vector<VssDiffAreaObservation>>::failure(observer_error(
              clonecore::ErrorCode::invalid_argument,
              E_INVALIDARG,
              L"VSS差分領域Windows binding重複検証",
              L"Snapshot、Source Volume、Snapshotデバイスは一意である必要があります"));
        }
      }
      snapshot_ids.push_back(parsed.value());
    }

    ComApartment apartment;
    const auto initialized = apartment.initialize();
    if (!initialized) {
      return clonecore::Result<
          std::vector<VssDiffAreaObservation>>::failure(
          initialized.error());
    }

    const auto security = initialize_vss_process_security();
    if (!security) {
      return clonecore::Result<
          std::vector<VssDiffAreaObservation>>::failure(
          security.error());
    }

    ComPtr<IVssSnapshotMgmt> management;
    const HRESULT result = CoCreateInstance(
        __uuidof(VssSnapshotMgmt),
        nullptr,
        CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(management.put()));
    if (FAILED(result) || management.get() == nullptr) {
      return clonecore::Result<
          std::vector<VssDiffAreaObservation>>::failure(observer_error(
          clonecore::ErrorCode::query_failed,
          FAILED(result) ? result : E_POINTER,
          L"VSS差分領域SnapshotMgmt作成",
          L"Snapshot属性のread-only management query境界を作成できません"));
    }

    std::vector<VssDiffAreaObservation> observations;
    observations.reserve(binding.snapshots.size());
    for (std::size_t index = 0U; index < binding.snapshots.size(); ++index) {
      const auto& expected = binding.snapshots[index];
      const auto& snapshot_id = snapshot_ids[index];

      auto source_before = probe_source_identity(
          impl_->options.probe_source_identity, expected);
      if (!source_before) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(
            source_before.error());
      }
      auto snapshot_before = query_and_validate_snapshot(
          *management.get(),
          snapshot_id,
          snapshot_set_id.value(),
          expected);
      if (!snapshot_before) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(
            snapshot_before.error());
      }
      auto observation = query_diff_area(
          *management.get(),
          binding,
          expected,
          snapshot_before.value(),
          source_before.value(),
          impl_->options.probe_backing_volume_space);
      if (!observation) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(
            observation.error());
      }

      auto snapshot_after = query_and_validate_snapshot(
          *management.get(),
          snapshot_id,
          snapshot_set_id.value(),
          expected);
      if (!snapshot_after ||
          !same_snapshot_identity(
              snapshot_before.value(), snapshot_after.value())) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(
            snapshot_after
                ? observer_error(
                      clonecore::ErrorCode::identity_mismatch,
                      VSS_E_OBJECT_NOT_FOUND,
                      L"VSS差分領域Snapshot差替え検証",
                      L"差分領域query中にSnapshot属性またはproviderが変化しました")
                : snapshot_after.error());
      }
      auto source_after = probe_source_identity(
          impl_->options.probe_source_identity, expected);
      if (!source_after || source_after.value() != source_before.value()) {
        return clonecore::Result<
            std::vector<VssDiffAreaObservation>>::failure(
            source_after
                ? observer_error(
                      clonecore::ErrorCode::identity_mismatch,
                      VSS_E_OBJECT_NOT_FOUND,
                      L"VSS差分領域Source差替え検証",
                      L"差分領域query中にsource identityが変化しました")
                : source_after.error());
      }

      observations.push_back(observation.take_value());
      if (impl_->options.logger != nullptr) {
        const auto& saved = observations.back();
        impl_->options.logger->info(
            L"VSS差分領域観測: used=" +
            std::to_wstring(saved.used_bytes) + L", allocated=" +
            std::to_wstring(saved.allocated_bytes) + L", maximum=" +
            std::to_wstring(saved.maximum_bytes));
      }
    }
    return clonecore::Result<
        std::vector<VssDiffAreaObservation>>::success(
        std::move(observations));
  } catch (...) {
    return clonecore::Result<
        std::vector<VssDiffAreaObservation>>::failure(observer_error(
        clonecore::ErrorCode::internal_error,
        E_UNEXPECTED,
        L"VSS差分領域Windows observer",
        L"Windows差分領域observerが予期しない例外を拒否しました"));
  }
}

}  // namespace ytec::vssrequester

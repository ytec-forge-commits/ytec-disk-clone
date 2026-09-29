#include "ytec/bootrepair/system_volume_mount.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::size_t kVolumeNameCharacters = 32U * 1024U;
constexpr std::size_t kExtentBufferBytes = 64U * 1024U;

clonecore::Error mount_error(
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

bool valid_drive_root(const std::wstring_view value) {
  return value.size() == 3U && std::iswalpha(value[0]) != 0 &&
      value[1] == L':' && value[2] == L'\\';
}

bool valid_volume_name(const std::wstring_view value) {
  constexpr std::wstring_view kPrefix = L"\\\\?\\Volume{";
  return value.size() > kPrefix.size() + 2U &&
      value.starts_with(kPrefix) && value.ends_with(L"}\\") &&
      value.find(L'\0') == std::wstring_view::npos;
}

bool same_location(
    const BootRepairVolumeLocation& left,
    const BootRepairVolumeLocation& right) {
  return left.disk_number == right.disk_number &&
      left.starting_offset == right.starting_offset &&
      left.extent_length == right.extent_length &&
      equals_case_insensitive(left.file_system, right.file_system);
}

class VolumeSearch final {
 public:
  explicit VolumeSearch(const HANDLE handle) noexcept : handle_(handle) {}
  ~VolumeSearch() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      FindVolumeClose(handle_);
    }
  }

  VolumeSearch(const VolumeSearch&) = delete;
  VolumeSearch& operator=(const VolumeSearch&) = delete;

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

clonecore::Result<BootRepairVolumeLocation> query_volume_location(
    const HANDLE volume,
    const std::wstring& volume_root,
    const BootRepairVolumeLocation* const trusted_expected = nullptr) {
  std::vector<std::byte> buffer(kExtentBufferBytes);
  DWORD bytes_returned = 0U;
  const bool extents_available = DeviceIoControl(
          volume,
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0U,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr) != FALSE;

  DWORD disk_number = 0U;
  LONGLONG starting_offset = 0;
  LONGLONG extent_length = 0;
  if (extents_available) {
    constexpr std::size_t kRequired =
        offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT);
    const auto* extents =
        reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    if (bytes_returned < kRequired || extents->NumberOfDiskExtents != 1U) {
      return clonecore::Result<BootRepairVolumeLocation>::failure(
          mount_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"一時システム領域の物理対応検証",
              L"単一物理ディスク上の通常パーティションだけを使用できます"));
    }
    disk_number = extents->Extents[0].DiskNumber;
    starting_offset = extents->Extents[0].StartingOffset.QuadPart;
    extent_length = extents->Extents[0].ExtentLength.QuadPart;
  } else {
    const DWORD extent_error = GetLastError();
    STORAGE_DEVICE_NUMBER device_number{};
    bytes_returned = 0U;
    const bool device_number_available = DeviceIoControl(
            volume,
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0U,
            &device_number,
            static_cast<DWORD>(sizeof(device_number)),
            &bytes_returned,
            nullptr) != FALSE;
    const bool disk_device_number_available =
        device_number_available &&
        bytes_returned >= sizeof(device_number) &&
        device_number.DeviceType == FILE_DEVICE_DISK;
    PARTITION_INFORMATION_EX partition{};
    bytes_returned = 0U;
    const bool partition_information_available = DeviceIoControl(
            volume,
            IOCTL_DISK_GET_PARTITION_INFO_EX,
            nullptr,
            0U,
            &partition,
            static_cast<DWORD>(sizeof(partition)),
            &bytes_returned,
            nullptr) != FALSE;
    if (!partition_information_available ||
        bytes_returned < sizeof(partition)) {
      return clonecore::Result<BootRepairVolumeLocation>::failure(
          mount_error(
              clonecore::ErrorCode::query_failed,
              partition_information_available
                  ? ERROR_INVALID_DATA
                  : GetLastError(),
              L"一時システム領域のパーティション対応代替取得",
              L"Volume extentsを利用できず、パーティション位置も取得できません"));
    }
    if (partition.PartitionNumber == 0U) {
      return clonecore::Result<BootRepairVolumeLocation>::failure(
          mount_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"一時システム領域のパーティション番号検証",
              L"パーティションAPIが通常パーティションを識別できません"));
    }
    const bool storage_partition_number_available =
        disk_device_number_available &&
        device_number.PartitionNumber != 0U &&
        device_number.PartitionNumber != MAXDWORD;
    if (storage_partition_number_available &&
        partition.PartitionNumber != device_number.PartitionNumber) {
      return clonecore::Result<BootRepairVolumeLocation>::failure(
          mount_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"一時システム領域のパーティション番号検証",
              L"代替API間でパーティション番号が一致しません"));
    }
    starting_offset = partition.StartingOffset.QuadPart;
    extent_length = partition.PartitionLength.QuadPart;
    if (disk_device_number_available) {
      disk_number = device_number.DeviceNumber;
    } else if (trusted_expected != nullptr && starting_offset >= 0 &&
               extent_length > 0 &&
               static_cast<std::uint64_t>(starting_offset) ==
                   trusted_expected->starting_offset &&
               static_cast<std::uint64_t>(extent_length) ==
                   trusted_expected->extent_length) {
      disk_number = trusted_expected->disk_number;
    } else {
      const DWORD fallback_error = device_number_available
          ? ERROR_INVALID_DATA
          : GetLastError();
      return clonecore::Result<BootRepairVolumeLocation>::failure(
          mount_error(
              clonecore::ErrorCode::query_failed,
              fallback_error == ERROR_SUCCESS ? extent_error : fallback_error,
              L"一時システム領域の物理対応代替検証",
              L"Volume GUID、パーティション範囲、またはディスク番号を安全に対応できません"
              L" (extentError=" + std::to_wstring(extent_error) +
              L", query=" +
              std::to_wstring(device_number_available ? 1U : 0U) +
              L", bytes=" + std::to_wstring(bytes_returned) +
              L", type=" + std::to_wstring(device_number.DeviceType) +
              L", disk=" + std::to_wstring(device_number.DeviceNumber) +
              L", partition=" +
              std::to_wstring(device_number.PartitionNumber) + L")"));
    }
  }
  if (starting_offset < 0 || extent_length <= 0) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        mount_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"一時システム領域の物理対応検証",
            L"単一物理ディスク上の通常パーティションだけを使用できます"));
  }

  std::array<wchar_t, MAX_PATH> file_system{};
  if (!GetVolumeInformationW(
          volume_root.c_str(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<BootRepairVolumeLocation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"一時システム領域のファイルシステム取得",
            GetLastError()));
  }
  return clonecore::Result<BootRepairVolumeLocation>::success(
      BootRepairVolumeLocation{
          .disk_number = disk_number,
          .starting_offset = static_cast<std::uint64_t>(starting_offset),
          .extent_length = static_cast<std::uint64_t>(extent_length),
          .file_system = file_system.data(),
      });
}

clonecore::Result<std::vector<std::wstring>> query_mount_points(
    const std::wstring& volume_name) {
  std::vector<wchar_t> buffer(kVolumeNameCharacters, L'\0');
  DWORD required = 0U;
  if (!GetVolumePathNamesForVolumeNameW(
          volume_name.c_str(),
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &required)) {
    return clonecore::Result<std::vector<std::wstring>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"システム領域の既存マウント先取得",
            GetLastError()));
  }
  std::vector<std::wstring> paths;
  const wchar_t* current = buffer.data();
  const wchar_t* const end = buffer.data() + buffer.size();
  while (current < end && *current != L'\0') {
    const std::wstring path(current);
    if (current + path.size() + 1U > end) {
      return clonecore::Result<std::vector<std::wstring>>::failure(
          mount_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"システム領域の既存マウント先検証",
              L"マルチ文字列応答がバッファ境界を超えています"));
    }
    paths.push_back(path);
    current += path.size() + 1U;
  }
  return clonecore::Result<std::vector<std::wstring>>::success(
      std::move(paths));
}

clonecore::Result<BootVolumeObservation> inspect_drive_root(
    const std::wstring& root,
    const std::wstring& expected_volume_name,
    const BootRepairVolumeLocation& expected_location) {
  if (!valid_drive_root(root) || !valid_volume_name(expected_volume_name)) {
    return clonecore::Result<BootVolumeObservation>::failure(
        mount_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DRIVE,
            L"一時システム領域のドライブ文字検証",
            L"一時割当先または期待Volume GUIDの形式が不正です"));
  }
  const auto mount_points = query_mount_points(expected_volume_name);
  if (!mount_points) {
    return clonecore::Result<BootVolumeObservation>::failure(
        mount_points.error());
  }
  if (mount_points.value().size() != 1U ||
      !equals_case_insensitive(mount_points.value().front(), root)) {
    return clonecore::Result<BootVolumeObservation>::failure(
        mount_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"一時システム領域のVolume GUID逆引き検証",
            L"期待Volume GUIDの割当先が計画したドライブ文字と一致しません"));
  }
  std::wstring open_path(expected_volume_name);
  if (!open_path.empty() && open_path.back() == L'\\') {
    open_path.pop_back();
  }
  clonecore::UniqueHandle volume(CreateFileW(
      open_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return clonecore::Result<BootVolumeObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"一時システム領域を読取り専用で再度開く",
            GetLastError()));
  }
  const auto location = query_volume_location(
      volume.get(), expected_volume_name, &expected_location);
  if (!location) {
    return clonecore::Result<BootVolumeObservation>::failure(
        location.error());
  }
  return clonecore::Result<BootVolumeObservation>::success(
      BootVolumeObservation{
          .volume_name = expected_volume_name,
          .location = location.value(),
          .mount_points = mount_points.value(),
      });
}

class WindowsSystemVolumeMountApi final : public ISystemVolumeMountApi {
 public:
  clonecore::Status attach(
      const std::wstring& temporary_root,
      const std::wstring& volume_name) override {
    if (!valid_drive_root(temporary_root) || !valid_volume_name(volume_name)) {
      return clonecore::Status::failure(mount_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"一時システム領域の割当引数",
          L"ドライブ文字またはVolume GUID形式が不正です"));
    }
    const wchar_t letter = static_cast<wchar_t>(
        std::towupper(temporary_root[0]));
    const DWORD bit = 1UL << static_cast<DWORD>(letter - L'A');
    const DWORD drives = GetLogicalDrives();
    if (drives == 0U || (drives & bit) != 0U) {
      return clonecore::Status::failure(mount_error(
          clonecore::ErrorCode::identity_mismatch,
          drives == 0U ? GetLastError() : ERROR_ALREADY_ASSIGNED,
          L"一時ドライブ文字の実行直前確認",
          L"計画したドライブ文字が使用中または状態不明です"));
    }
    if (!SetVolumeMountPointW(
            temporary_root.c_str(), volume_name.c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"システム領域への一時ドライブ文字割当",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  clonecore::Result<BootVolumeObservation> inspect(
      const std::wstring& temporary_root,
      const std::wstring& expected_volume_name,
      const BootRepairVolumeLocation& expected_location) override {
    return inspect_drive_root(
        temporary_root, expected_volume_name, expected_location);
  }

  clonecore::Status detach(
      const std::wstring& temporary_root,
      const std::wstring& expected_volume_name) override {
    if (!valid_drive_root(temporary_root) ||
        !valid_volume_name(expected_volume_name)) {
      return clonecore::Status::failure(mount_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"一時ドライブ文字の解除引数",
          L"ドライブ文字または期待Volume GUIDの形式が不正です"));
    }
    const auto before = query_mount_points(expected_volume_name);
    if (!before) {
      return clonecore::Status::failure(before.error());
    }
    if (before.value().size() != 1U ||
        !equals_case_insensitive(before.value().front(), temporary_root)) {
      return clonecore::Status::failure(mount_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"一時ドライブ文字の解除前逆引き検証",
          L"期待Volume GUIDの唯一の割当先が計画したドライブ文字ではありません"));
    }
    if (!DeleteVolumeMountPointW(temporary_root.c_str())) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"一時システム領域のドライブ文字解除",
          GetLastError()));
    }
    const auto remaining = query_mount_points(expected_volume_name);
    if (!remaining) {
      return clonecore::Status::failure(remaining.error());
    }
    const bool temporary_root_remains = std::any_of(
        remaining.value().begin(),
        remaining.value().end(),
        [&](const std::wstring& mount_point) {
          return equals_case_insensitive(mount_point, temporary_root);
        });
    if (temporary_root_remains) {
      return clonecore::Status::failure(mount_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_ALREADY_ASSIGNED,
          L"一時ドライブ文字の解除確認",
          L"解除後も期待Volume GUIDへ一時ドライブ文字が残っています"));
    }
    return clonecore::success_status();
  }

};

}  // namespace

clonecore::Result<TemporarySystemVolumeMountPlan>
plan_temporary_system_volume_mount(
    const diskmodel::DiskInfo& disk,
    const BcdBootFirmware firmware,
    const std::vector<BootVolumeObservation>& volumes,
    const std::wstring_view unavailable_drive_letters) {
  const diskmodel::PartitionStyle expected_style =
      firmware == BcdBootFirmware::uefi
      ? diskmodel::PartitionStyle::gpt
      : diskmodel::PartitionStyle::mbr;
  if ((firmware != BcdBootFirmware::uefi &&
       firmware != BcdBootFirmware::bios) ||
      disk.partition_style != expected_style || disk.is_system_disk ||
      !disk.read_only.has_value() || !disk.removable.has_value() ||
      disk.read_only.value() || disk.removable.value()) {
    return clonecore::Result<TemporarySystemVolumeMountPlan>::failure(
        mount_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_ACCESS_DENIED,
            L"未割当システム領域のディスク条件",
            L"固定・書込み可能でファームウェア形式が一致する非システムディスクだけを対象にできます"));
  }

  std::vector<const diskmodel::PartitionInfo*> candidates;
  for (const auto& partition : disk.partitions) {
    const bool matches = firmware == BcdBootFirmware::uefi
        ? partition.style == diskmodel::PartitionStyle::gpt &&
            equals_case_insensitive(partition.type, kEfiPartitionType)
        : partition.style == diskmodel::PartitionStyle::mbr &&
            partition.bootable &&
            equals_case_insensitive(partition.type, L"0x07");
    if (matches) {
      candidates.push_back(&partition);
    }
  }
  if (candidates.size() != 1U) {
    return clonecore::Result<TemporarySystemVolumeMountPlan>::failure(
        mount_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"未割当システム領域の一意選択",
            L"ESPまたはBIOS Active NTFS領域を一つだけ特定できません"));
  }
  const auto& partition = *candidates.front();

  std::vector<const BootVolumeObservation*> matching_volumes;
  for (const auto& volume : volumes) {
    if (volume.location.disk_number == disk.disk_number &&
        volume.location.starting_offset == partition.offset_bytes &&
        volume.location.extent_length == partition.size_bytes) {
      matching_volumes.push_back(&volume);
    }
  }
  if (matching_volumes.size() != 1U) {
    return clonecore::Result<TemporarySystemVolumeMountPlan>::failure(
        mount_error(
            clonecore::ErrorCode::identity_mismatch,
            matching_volumes.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
            L"未割当システム領域のVolume GUID対応",
            L"パーティション範囲に一致するVolume GUIDを一つだけ特定できません"));
  }
  const auto& volume = *matching_volumes.front();
  const std::wstring_view expected_file_system =
      firmware == BcdBootFirmware::uefi ? L"FAT32" : L"NTFS";
  if (!valid_volume_name(volume.volume_name) ||
      !equals_case_insensitive(
          volume.location.file_system, expected_file_system) ||
      !volume.mount_points.empty()) {
    return clonecore::Result<TemporarySystemVolumeMountPlan>::failure(
        mount_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_ALREADY_ASSIGNED,
            L"未割当システム領域の状態確認",
            L"対応Volume GUID、ファイルシステム、未割当状態のいずれかが一致しません"));
  }

  std::array<bool, 26> unavailable{};
  for (const wchar_t character : unavailable_drive_letters) {
    if (std::iswalpha(character) != 0) {
      const wchar_t upper = static_cast<wchar_t>(std::towupper(character));
      unavailable[static_cast<std::size_t>(upper - L'A')] = true;
    }
  }
  for (const auto& observed_volume : volumes) {
    for (const auto& mount_point : observed_volume.mount_points) {
      if (valid_drive_root(mount_point)) {
        const wchar_t upper =
            static_cast<wchar_t>(std::towupper(mount_point[0]));
        unavailable[static_cast<std::size_t>(upper - L'A')] = true;
      }
    }
  }
  unavailable[static_cast<std::size_t>(L'X' - L'A')] = true;
  wchar_t selected = L'\0';
  for (wchar_t letter = L'Y'; letter >= L'D'; --letter) {
    if (!unavailable[static_cast<std::size_t>(letter - L'A')]) {
      selected = letter;
      break;
    }
  }
  if (selected == L'\0') {
    return clonecore::Result<TemporarySystemVolumeMountPlan>::failure(
        mount_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NO_MORE_ITEMS,
            L"未割当システム領域の一時ドライブ文字選択",
            L"安全に使用できる未使用ドライブ文字がありません"));
  }

  return clonecore::Result<TemporarySystemVolumeMountPlan>::success(
      TemporarySystemVolumeMountPlan{
          .firmware = firmware,
          .disk_number = disk.disk_number,
          .partition_number = partition.number,
          .volume_name = volume.volume_name,
          .temporary_root = std::wstring(1U, selected) + L":\\",
          .expected_location = volume.location,
      });
}

TemporarySystemVolumeMount::TemporarySystemVolumeMount(
    std::wstring root,
    std::wstring volume_name,
    ISystemVolumeMountApi& api) noexcept
    : root_(std::move(root)),
      volume_name_(std::move(volume_name)),
      api_(&api),
      mounted_(true) {}

TemporarySystemVolumeMount::~TemporarySystemVolumeMount() noexcept {
  if (mounted_ && api_ != nullptr) {
    try {
      static_cast<void>(api_->detach(root_, volume_name_));
    } catch (...) {
    }
  }
}

TemporarySystemVolumeMount::TemporarySystemVolumeMount(
    TemporarySystemVolumeMount&& other) noexcept
    : root_(std::move(other.root_)),
      volume_name_(std::move(other.volume_name_)),
      api_(other.api_),
      mounted_(other.mounted_) {
  other.api_ = nullptr;
  other.mounted_ = false;
}

clonecore::Result<TemporarySystemVolumeMount>
TemporarySystemVolumeMount::acquire(
    const TemporarySystemVolumeMountPlan& plan,
    ISystemVolumeMountApi& api) {
  const auto attached = api.attach(plan.temporary_root, plan.volume_name);
  if (!attached) {
    return clonecore::Result<TemporarySystemVolumeMount>::failure(
        attached.error());
  }
  const auto observed = api.inspect(
      plan.temporary_root, plan.volume_name, plan.expected_location);
  if (!observed ||
      !equals_case_insensitive(
          observed.value().volume_name, plan.volume_name) ||
      !same_location(
          observed.value().location, plan.expected_location)) {
    const auto detached =
        api.detach(plan.temporary_root, plan.volume_name);
    if (!detached) {
      return clonecore::Result<TemporarySystemVolumeMount>::failure(
          detached.error());
    }
    if (!observed) {
      return clonecore::Result<TemporarySystemVolumeMount>::failure(
          observed.error());
    }
    return clonecore::Result<TemporarySystemVolumeMount>::failure(
        mount_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"一時システム領域の割当後再識別",
            L"割当後のVolume GUIDまたは物理範囲が計画と一致しません"));
  }
  return clonecore::Result<TemporarySystemVolumeMount>::success(
      TemporarySystemVolumeMount(
          plan.temporary_root, plan.volume_name, api));
}

clonecore::Status TemporarySystemVolumeMount::release() {
  if (!mounted_ || api_ == nullptr) {
    return clonecore::success_status();
  }
  const auto detached = api_->detach(root_, volume_name_);
  if (!detached) {
    return detached;
  }
  mounted_ = false;
  return clonecore::success_status();
}

clonecore::Result<std::vector<BootVolumeObservation>>
enumerate_windows_boot_volumes_read_only() {
  std::vector<wchar_t> volume_name(kVolumeNameCharacters, L'\0');
  const HANDLE raw_search = FindFirstVolumeW(
      volume_name.data(), static_cast<DWORD>(volume_name.size()));
  if (raw_search == INVALID_HANDLE_VALUE) {
    return clonecore::Result<std::vector<BootVolumeObservation>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::enumeration_failed,
            L"未割当システム領域のVolume GUID列挙開始",
            GetLastError()));
  }
  VolumeSearch search(raw_search);
  std::vector<BootVolumeObservation> observations;
  for (;;) {
    const std::wstring safe_name(volume_name.data());
    if (!valid_volume_name(safe_name)) {
      return clonecore::Result<std::vector<BootVolumeObservation>>::failure(
          mount_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_NAME,
              L"未割当システム領域のVolume GUID形式",
              L"Windowsボリューム列挙が不正なVolume GUIDを返しました"));
    }
    std::wstring open_path = safe_name;
    open_path.pop_back();
    clonecore::UniqueHandle volume(CreateFileW(
        open_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!volume) {
      return clonecore::Result<std::vector<BootVolumeObservation>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"未割当システム領域のVolume GUID読取り",
              GetLastError()));
    }
    STORAGE_DEVICE_NUMBER device_number{};
    DWORD device_number_bytes = 0U;
    const bool device_number_available = DeviceIoControl(
            volume.get(),
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0U,
            &device_number,
            static_cast<DWORD>(sizeof(device_number)),
            &device_number_bytes,
            nullptr) != FALSE;
    const bool explicitly_non_disk =
        device_number_available &&
        device_number_bytes >= sizeof(device_number) &&
        device_number.DeviceType != FILE_DEVICE_DISK;
    if (!explicitly_non_disk) {
      const auto location = query_volume_location(volume.get(), safe_name);
      if (!location) {
        return clonecore::Result<
            std::vector<BootVolumeObservation>>::failure(location.error());
      }
      const auto mount_points = query_mount_points(safe_name);
      if (!mount_points) {
        return clonecore::Result<
            std::vector<BootVolumeObservation>>::failure(
            mount_points.error());
      }
      observations.push_back(BootVolumeObservation{
          .volume_name = safe_name,
          .location = location.value(),
          .mount_points = mount_points.value(),
      });
    }

    std::fill(volume_name.begin(), volume_name.end(), L'\0');
    if (!FindNextVolumeW(
            raw_search,
            volume_name.data(),
            static_cast<DWORD>(volume_name.size()))) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_NO_MORE_FILES) {
        break;
      }
      return clonecore::Result<std::vector<BootVolumeObservation>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"未割当システム領域のVolume GUID列挙反復",
              native_code));
    }
  }
  return clonecore::Result<std::vector<BootVolumeObservation>>::success(
      std::move(observations));
}

std::unique_ptr<ISystemVolumeMountApi>
make_windows_system_volume_mount_api() {
  return std::make_unique<WindowsSystemVolumeMountApi>();
}

}  // namespace ytec::bootrepair

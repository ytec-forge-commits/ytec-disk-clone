#include "ytec/windowsapp/usb_volume_mapping.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/windowsapp/rescue_media_storage.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kInitialExtentBufferBytes = 4096U;
constexpr std::size_t kMaximumExtentBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumExtentCount = 256U;

clonecore::Error mapping_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"レスキューUSBドライブ文字の読み取り専用照合",
      .message = std::move(message),
  };
}

bool equals_ascii_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(
             left.begin(),
             left.end(),
             right.begin(),
             [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
             });
}

bool is_ascii_drive_letter(const wchar_t letter) noexcept {
  return (letter >= L'A' && letter <= L'Z') ||
         (letter >= L'a' && letter <= L'z');
}

wchar_t upper_drive_letter(const wchar_t letter) noexcept {
  return letter >= L'a' && letter <= L'z'
             ? static_cast<wchar_t>(letter - L'a' + L'A')
             : letter;
}

clonecore::Result<std::vector<DriveLetterDiskExtent>>
query_drive_extents_read_only(const HANDLE volume) {
  std::vector<std::byte> buffer(kInitialExtentBufferBytes);
  for (;;) {
    DWORD bytes_returned = 0U;
    if (DeviceIoControl(
            volume,
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            nullptr,
            0U,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned,
            nullptr) != FALSE) {
      constexpr std::size_t header_size =
          offsetof(VOLUME_DISK_EXTENTS, Extents);
      if (bytes_returned < header_size) {
        return clonecore::Result<
            std::vector<DriveLetterDiskExtent>>::failure(
            mapping_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"ボリューム範囲応答が短すぎます"));
      }
      const auto* native =
          reinterpret_cast<const VOLUME_DISK_EXTENTS*>(
              buffer.data());
      if (native->NumberOfDiskExtents == 0U ||
          native->NumberOfDiskExtents > kMaximumExtentCount) {
        return clonecore::Result<
            std::vector<DriveLetterDiskExtent>>::failure(
            mapping_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"ボリューム範囲件数が不正です"));
      }
      const std::size_t required =
          header_size +
          static_cast<std::size_t>(native->NumberOfDiskExtents) *
              sizeof(DISK_EXTENT);
      if (required > bytes_returned) {
        return clonecore::Result<
            std::vector<DriveLetterDiskExtent>>::failure(
            mapping_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"ボリューム範囲応答に全要素がありません"));
      }

      std::vector<DriveLetterDiskExtent> extents;
      extents.reserve(native->NumberOfDiskExtents);
      for (DWORD index = 0U;
           index < native->NumberOfDiskExtents;
           ++index) {
        const auto& extent = native->Extents[index];
        if (extent.StartingOffset.QuadPart < 0 ||
            extent.ExtentLength.QuadPart <= 0) {
          // Empty card-reader slots can have drive letters and report a
          // single zero-length sentinel extent. They are not usable volume
          // evidence, but must not prevent an unrelated selected USB from
          // being resolved. Returning an empty set makes the caller skip this
          // volume; if the selected USB itself reports only such an extent,
          // the resolver still fails closed because no mapping is found.
          extents.clear();
          return clonecore::Result<
              std::vector<DriveLetterDiskExtent>>::success(
              std::move(extents));
        }
        extents.push_back({
            .disk_number = extent.DiskNumber,
            .starting_offset = static_cast<std::uint64_t>(
                extent.StartingOffset.QuadPart),
            .length = static_cast<std::uint64_t>(
                extent.ExtentLength.QuadPart),
        });
      }
      return clonecore::Result<
          std::vector<DriveLetterDiskExtent>>::success(
          std::move(extents));
    }

    const DWORD native_code = GetLastError();
    if ((native_code == ERROR_MORE_DATA ||
         native_code == ERROR_INSUFFICIENT_BUFFER) &&
        buffer.size() < kMaximumExtentBufferBytes) {
      buffer.resize(
          (std::min)(
              buffer.size() * 2U,
              kMaximumExtentBufferBytes));
      continue;
    }
    return clonecore::Result<
        std::vector<DriveLetterDiskExtent>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ドライブ文字の物理ディスク範囲取得",
            native_code));
  }
}

}  // namespace

clonecore::Result<RescueUsbDriveLetterResolution>
resolve_rescue_usb_drive_letter_for_plan(
    const diskmodel::DiskInfo& target,
    const std::span<const DriveLetterVolume> volumes,
    const RescueUsbStoragePlan& reviewed_plan,
    const RescueUsbDestinationVerificationPoint verification_point) {
  const auto binding =
      validate_rescue_usb_storage_plan_binding(reviewed_plan);
  if (!binding) {
    return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
        binding.error());
  }
  const auto observed = diskmodel::make_stable_disk_identity(target, false);
  if (!observed) {
    return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
        observed.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      reviewed_plan.expected_target, observed.value(),
      L"レスキューUSBドライブ文字計画");
  if (!identity || target.disk_number !=
                       reviewed_plan.expected_target.disk_number) {
    return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
        identity
            ? mapping_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_NOT_CONNECTED,
                  L"USBのディスク番号がレビュー時から変化しました")
            : identity.error());
  }

  const auto current_layout = make_rescue_usb_canonical_layout(target);
  if (verification_point ==
      RescueUsbDestinationVerificationPoint::before_write) {
    if (current_layout != reviewed_plan.reviewed_layout) {
      return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"USBの完全レイアウトがレビュー時から変化しました"));
    }
    if (reviewed_plan.mode ==
        RescueUsbProvisioningMode::preserve_data_refresh) {
      auto resolution = resolve_rescue_usb_drive_letter(target, volumes);
      if (!resolution || resolution.value().partition_number != 1U ||
          resolution.value().drive_letter_was_unassigned) {
        return resolution
            ? clonecore::Result<RescueUsbDriveLetterResolution>::failure(
                  mapping_error(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_NOT_FOUND,
                      L"保持更新する起動領域を一意に解決できません"))
            : resolution;
      }
      return resolution;
    }
    const auto plan_status = validate_rescue_usb_storage_plan(
        reviewed_plan, target, nullptr);
    if (!plan_status) {
      return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
          plan_status.error());
    }
    std::array<bool, 26U> occupied{};
    for (const auto& volume : volumes) {
      if (!is_ascii_drive_letter(volume.drive_letter)) {
        return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
            mapping_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DRIVE,
                L"Windowsから不正なローカルドライブ文字が返されました"));
      }
      const wchar_t letter = upper_drive_letter(volume.drive_letter);
      occupied[static_cast<std::size_t>(letter - L'A')] = true;
    }
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
      if (!occupied[static_cast<std::size_t>(letter - L'A')]) {
        return clonecore::Result<RescueUsbDriveLetterResolution>::success({
            .target_identity = observed.value(),
            .drive_letter = letter,
            .root_path = std::wstring{letter, L':', L'\\'},
            .partition_number = 0U,
            .extent_start = 0U,
            .extent_length = 0U,
            .drive_letter_was_unassigned = true,
            .physical_write_started = false,
        });
      }
    }
    return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NO_MORE_FILES,
            L"USB作成に割り当てられる空きドライブ文字がありません"));
  }

  const auto completed = validate_rescue_usb_completed_layout(
      current_layout, target.size_bytes);
  if (!completed ||
      (reviewed_plan.mode ==
           RescueUsbProvisioningMode::preserve_data_refresh &&
       current_layout != reviewed_plan.reviewed_layout)) {
    return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
        completed
            ? mapping_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_NOT_CONNECTED,
                  L"保持更新後の完全レイアウトがレビュー値と一致しません")
            : completed.error());
  }
  auto resolution = resolve_rescue_usb_drive_letter(target, volumes);
  if (!resolution || resolution.value().partition_number != 1U ||
      resolution.value().drive_letter_was_unassigned) {
    return resolution
        ? clonecore::Result<RescueUsbDriveLetterResolution>::failure(
              mapping_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_NOT_FOUND,
                  L"完成したUSBの起動領域を一意に解決できません"))
        : resolution;
  }
  return resolution;
}

clonecore::Result<RescueUsbDriveLetterResolution>
resolve_rescue_usb_drive_letter(
    const diskmodel::DiskInfo& target,
    const std::span<const DriveLetterVolume> volumes) {
  if (target.is_system_disk) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::access_denied,
            ERROR_ACCESS_DENIED,
            L"WindowsのシステムディスクはUSB作成先にできません"));
  }
  if (!target.removable.has_value() ||
      !target.read_only.has_value() ||
      !target.offline.has_value()) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"USBの取り外し可能・読み取り専用・オンライン状態が不明です"));
  }
  if (!target.removable.value() ||
      !equals_ascii_case_insensitive(target.bus_type, L"USB")) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"取り外し可能なUSBディスクだけを照合できます"));
  }
  if (target.read_only.value() || target.offline.value()) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::access_denied,
            ERROR_ACCESS_DENIED,
            L"読み取り専用またはオフラインのUSBは作成先にできません"));
  }

  auto identity =
      diskmodel::make_stable_disk_identity(target, false);
  if (!identity) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(identity.error());
  }

  if (target.partitions.empty()) {
    if (target.partition_style != diskmodel::PartitionStyle::raw &&
        target.partition_style != diskmodel::PartitionStyle::gpt &&
        target.partition_style != diskmodel::PartitionStyle::mbr) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"区画のないUSBのパーティション形式が不明です"));
    }
    std::array<bool, 26U> occupied{};
    for (const auto& volume : volumes) {
      if (!is_ascii_drive_letter(volume.drive_letter)) {
        return clonecore::Result<
            RescueUsbDriveLetterResolution>::failure(
            mapping_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DRIVE,
                L"Windowsから不正なローカルドライブ文字が返されました"));
      }
      const wchar_t drive_letter =
          upper_drive_letter(volume.drive_letter);
      occupied[static_cast<std::size_t>(drive_letter - L'A')] = true;
      if (std::any_of(
              volume.extents.begin(),
              volume.extents.end(),
              [&](const auto& extent) {
                return extent.disk_number == target.disk_number;
              })) {
        return clonecore::Result<
            RescueUsbDriveLetterResolution>::failure(
            mapping_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"区画のないUSBに既存ドライブ文字の範囲が残っています"));
      }
    }
    for (wchar_t drive_letter = L'D';
         drive_letter <= L'Z';
         ++drive_letter) {
      if (occupied[static_cast<std::size_t>(drive_letter - L'A')]) {
        continue;
      }
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::success({
          .target_identity = identity.value(),
          .drive_letter = drive_letter,
          .root_path = std::wstring{drive_letter, L':', L'\\'},
          .partition_number = 0U,
          .extent_start = 0U,
          .extent_length = 0U,
          .drive_letter_was_unassigned = true,
          .physical_write_started = false,
      });
    }
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NO_MORE_FILES,
            L"USB作成に割り当てられる空きドライブ文字がありません"));
  }
  if (target.partitions.size() > 2U) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"0～1領域または検証済みY-TEC 2領域USBだけを照合できます"));
  }

  const diskmodel::PartitionInfo* selected_partition = nullptr;
  const diskmodel::PartitionInfo* data_partition = nullptr;
  if (target.partitions.size() == 1U) {
    selected_partition = &target.partitions.front();
  } else {
    if (target.partition_style != diskmodel::PartitionStyle::mbr) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"2領域USBはMBRのY-TEC媒体だけを照合できます"));
    }
    for (const auto& partition : target.partitions) {
      if (partition.number == 1U) {
        selected_partition = &partition;
      } else if (partition.number == 2U) {
        data_partition = &partition;
      } else {
        return clonecore::Result<
            RescueUsbDriveLetterResolution>::failure(
            mapping_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"2領域USBのパーティション番号がY-TEC構成ではありません"));
      }
    }
    if (selected_partition == nullptr || data_partition == nullptr ||
        selected_partition->style != diskmodel::PartitionStyle::mbr ||
        data_partition->style != diskmodel::PartitionStyle::mbr ||
        selected_partition->size_bytes != kRescueUsbBootPartitionBytes ||
        !selected_partition->bootable || data_partition->bootable ||
        data_partition->offset_bytes !=
            selected_partition->offset_bytes +
                selected_partition->size_bytes ||
        data_partition->size_bytes !=
            target.size_bytes - data_partition->offset_bytes) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"2領域USBが4GiB起動領域＋残容量データ領域ではありません"));
    }
  }

  std::optional<RescueUsbDriveLetterResolution> resolution;
  bool data_mapping_seen = false;
  for (const auto& volume : volumes) {
    const bool references_target = std::any_of(
        volume.extents.begin(),
        volume.extents.end(),
        [&](const auto& extent) {
          return extent.disk_number == target.disk_number;
        });
    if (!references_target) {
      continue;
    }
    if (!is_ascii_drive_letter(volume.drive_letter)) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DRIVE,
              L"対象USBに不正なドライブ文字が対応しています"));
    }
    if (volume.extents.size() != 1U) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"複数ディスクへまたがるボリュームはUSB作成先にできません"));
    }
    const auto& extent = volume.extents.front();
    if (extent.length == 0U ||
        extent.starting_offset > target.size_bytes ||
        extent.length >
            target.size_bytes - extent.starting_offset) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"ドライブ文字の範囲が選択USBの容量外です"));
    }
    const auto partition = std::find_if(
        target.partitions.begin(),
        target.partitions.end(),
        [&](const auto& candidate) {
          return candidate.offset_bytes == extent.starting_offset &&
                 extent.length == candidate.size_bytes;
        });
    if (partition == target.partitions.end()) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"ドライブ文字を選択USBのパーティションへ対応付けできません"));
    }
    if (&*partition != selected_partition) {
      if (data_partition == nullptr || &*partition != data_partition ||
          data_mapping_seen) {
        return clonecore::Result<
            RescueUsbDriveLetterResolution>::failure(
            mapping_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DUP_NAME,
                L"選択USBのデータ領域対応が曖昧です"));
      }
      data_mapping_seen = true;
      continue;
    }
    if (resolution.has_value()) {
      return clonecore::Result<
          RescueUsbDriveLetterResolution>::failure(
          mapping_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              L"選択USBに複数のドライブ文字があり一意に確定できません"));
    }

    const wchar_t drive_letter =
        upper_drive_letter(volume.drive_letter);
    resolution = RescueUsbDriveLetterResolution{
        .target_identity = identity.value(),
        .drive_letter = drive_letter,
        .root_path = std::wstring{drive_letter, L':', L'\\'},
        .partition_number = partition->number,
        .extent_start = extent.starting_offset,
        .extent_length = extent.length,
        .drive_letter_was_unassigned = false,
        .physical_write_started = false,
    };
  }

  if (!resolution.has_value() ||
      (data_partition != nullptr && !data_mapping_seen)) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(
        mapping_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            data_partition != nullptr
                ? L"選択USBの起動／データ領域を一意に対応付けできません"
                : L"選択USBへ一意に対応するローカルドライブ文字がありません"));
  }
  return clonecore::Result<
      RescueUsbDriveLetterResolution>::success(
      std::move(resolution.value()));
}

clonecore::Result<std::vector<DriveLetterVolume>>
enumerate_windows_drive_letter_volumes_read_only() {
  const DWORD required = GetLogicalDriveStringsW(0U, nullptr);
  if (required == 0U) {
    return clonecore::Result<
        std::vector<DriveLetterVolume>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::enumeration_failed,
            L"ローカルドライブ文字列挙",
            GetLastError()));
  }
  std::vector<wchar_t> drive_strings(
      static_cast<std::size_t>(required) + 1U, L'\0');
  const DWORD copied = GetLogicalDriveStringsW(
      static_cast<DWORD>(drive_strings.size()),
      drive_strings.data());
  if (copied == 0U || copied >= drive_strings.size()) {
    return clonecore::Result<
        std::vector<DriveLetterVolume>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::enumeration_failed,
            L"ローカルドライブ文字列挙",
            copied == 0U ? GetLastError() : ERROR_MORE_DATA));
  }

  std::vector<DriveLetterVolume> volumes;
  for (const wchar_t* root = drive_strings.data();
       *root != L'\0';
       root += std::wcslen(root) + 1U) {
    const std::wstring_view root_view(root);
    if (root_view.size() != 3U ||
        !is_ascii_drive_letter(root_view[0]) ||
        root_view[1] != L':' || root_view[2] != L'\\') {
      return clonecore::Result<
          std::vector<DriveLetterVolume>>::failure(
          mapping_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DRIVE,
              L"Windowsから不正なローカルドライブ文字列が返されました"));
    }
    const UINT drive_type = GetDriveTypeW(root);
    if (drive_type != DRIVE_FIXED &&
        drive_type != DRIVE_REMOVABLE) {
      volumes.push_back({
          .drive_letter = upper_drive_letter(root_view[0]),
          .extents = {},
      });
      continue;
    }

    const std::wstring device_path =
        L"\\\\.\\" + std::wstring(1U, root_view[0]) + L":";
    clonecore::UniqueHandle volume(CreateFileW(
        device_path.c_str(),
        0U,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!volume) {
      return clonecore::Result<
          std::vector<DriveLetterVolume>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"ローカルドライブ範囲の読み取り専用照会",
              GetLastError()));
    }
    auto extents = query_drive_extents_read_only(volume.get());
    if (!extents) {
      return clonecore::Result<
          std::vector<DriveLetterVolume>>::failure(extents.error());
    }
    if (extents.value().empty()) {
      volumes.push_back({
          .drive_letter = upper_drive_letter(root_view[0]),
          .extents = {},
      });
      continue;
    }
    volumes.push_back({
        .drive_letter = upper_drive_letter(root_view[0]),
        .extents = extents.take_value(),
    });
  }
  return clonecore::Result<
      std::vector<DriveLetterVolume>>::success(std::move(volumes));
}

clonecore::Result<RescueUsbDriveLetterResolution>
resolve_windows_rescue_usb_drive_letter_read_only(
    const diskmodel::DiskInfo& target) {
  auto volumes = enumerate_windows_drive_letter_volumes_read_only();
  if (!volumes) {
    return clonecore::Result<
        RescueUsbDriveLetterResolution>::failure(volumes.error());
  }
  return resolve_rescue_usb_drive_letter(target, volumes.value());
}

clonecore::Result<RescueUsbDriveLetterResolution>
resolve_windows_rescue_usb_drive_letter_for_plan_read_only(
    const diskmodel::DiskInfo& target,
    const RescueUsbStoragePlan& reviewed_plan,
    const RescueUsbDestinationVerificationPoint verification_point) {
  auto volumes = enumerate_windows_drive_letter_volumes_read_only();
  if (!volumes) {
    return clonecore::Result<RescueUsbDriveLetterResolution>::failure(
        volumes.error());
  }
  return resolve_rescue_usb_drive_letter_for_plan(
      target, volumes.value(), reviewed_plan, verification_point);
}

}  // namespace ytec::windowsapp

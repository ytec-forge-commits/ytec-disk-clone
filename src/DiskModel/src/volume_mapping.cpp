#include "ytec/diskmodel/physical_disk.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::diskmodel {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::UniqueHandle;
using clonecore::VolumeBitmapBinding;

constexpr std::size_t kVolumeNameCharacters = 32U * 1024U;
constexpr std::size_t kExtentBufferBytes = 64U * 1024U;
constexpr std::size_t kMaximumExtentCount = 256;

struct ExpectedVolumePartition final {
  std::uint32_t table_index{};
  std::uint64_t offset_bytes{};
};

Error mapping_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
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

Result<VOLUME_DISK_EXTENTS> query_single_extent(const HANDLE volume) {
  std::vector<std::byte> buffer(kExtentBufferBytes);
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          volume,
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr)) {
    const DWORD extent_error = GetLastError();
    if (extent_error != ERROR_INVALID_FUNCTION) {
      return Result<VOLUME_DISK_EXTENTS>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              L"ボリュームの物理ディスク対応取得",
              extent_error));
    }

    // WinPE storage drivers can reject volume extents even for an ordinary
    // single-disk partition. Accept the fallback only when the independent
    // storage-number and partition-information APIs agree on a disk-backed
    // partition with a valid range.
    STORAGE_DEVICE_NUMBER device_number{};
    DWORD device_bytes = 0U;
    const bool device_number_available = DeviceIoControl(
            volume,
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0U,
            &device_number,
            static_cast<DWORD>(sizeof(device_number)),
            &device_bytes,
            nullptr) != FALSE;
    const DWORD device_number_error =
        device_number_available ? ERROR_SUCCESS : GetLastError();
    if (!device_number_available || device_bytes < sizeof(device_number) ||
        device_number.DeviceType != FILE_DEVICE_DISK) {
      const DWORD native_code = !device_number_available
          ? device_number_error
          : ERROR_INVALID_DATA;
      return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
          ErrorCode::query_failed,
          native_code == ERROR_SUCCESS ? extent_error : native_code,
          L"ボリュームの物理ディスク対応代替取得",
          L"ディスクデバイス番号を安全に取得できません"
          L" (extentError=" + std::to_wstring(extent_error) +
          L", query=" +
          std::to_wstring(device_number_available ? 1U : 0U) +
          L", queryError=" + std::to_wstring(device_number_error) +
          L", bytes=" + std::to_wstring(device_bytes) +
          L", type=" + std::to_wstring(device_number.DeviceType) +
          L", disk=" + std::to_wstring(device_number.DeviceNumber) +
          L", partition=" +
          std::to_wstring(device_number.PartitionNumber) + L")"));
    }

    PARTITION_INFORMATION_EX partition{};
    DWORD partition_bytes = 0U;
    if (!DeviceIoControl(
            volume,
            IOCTL_DISK_GET_PARTITION_INFO_EX,
            nullptr,
            0U,
            &partition,
            static_cast<DWORD>(sizeof(partition)),
            &partition_bytes,
            nullptr) ||
        partition_bytes < sizeof(partition) ||
        partition.PartitionNumber == 0U ||
        partition.StartingOffset.QuadPart < 0 ||
        partition.PartitionLength.QuadPart <= 0) {
      const DWORD native_code = GetLastError();
      return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
          ErrorCode::query_failed,
          native_code == ERROR_SUCCESS ? ERROR_INVALID_DATA : native_code,
          L"ボリュームのパーティション対応代替取得",
          L"通常パーティションの範囲を安全に取得できません"));
    }

    const bool storage_partition_number_available =
        device_number.PartitionNumber != 0U &&
        device_number.PartitionNumber != MAXDWORD;
    if (storage_partition_number_available &&
        device_number.PartitionNumber != partition.PartitionNumber) {
      return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
          ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ボリュームのパーティション対応代替検証",
          L"代替API間でパーティション番号が一致しません"));
    }

    VOLUME_DISK_EXTENTS single{};
    single.NumberOfDiskExtents = 1U;
    single.Extents[0].DiskNumber = device_number.DeviceNumber;
    single.Extents[0].StartingOffset = partition.StartingOffset;
    single.Extents[0].ExtentLength = partition.PartitionLength;
    return Result<VOLUME_DISK_EXTENTS>::success(single);
  }
  constexpr std::size_t header_size =
      offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (bytes_returned < header_size) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ボリュームの物理ディスク対応検証",
        L"ディスク範囲応答が短すぎます"));
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents == 0 ||
      extents->NumberOfDiskExtents > kMaximumExtentCount) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ボリュームの物理ディスク対応検証",
        L"ディスク範囲件数が不正です"));
  }
  const std::size_t required = header_size +
      static_cast<std::size_t>(extents->NumberOfDiskExtents) *
          sizeof(DISK_EXTENT);
  if (required > bytes_returned) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ボリュームの物理ディスク対応検証",
        L"ディスク範囲応答に全要素がありません"));
  }
  if (extents->NumberOfDiskExtents != 1) {
    return Result<VOLUME_DISK_EXTENTS>::failure(mapping_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"ボリュームの物理ディスク対応検証",
        L"複数ディスクへまたがるボリュームは対応しません"));
  }
  VOLUME_DISK_EXTENTS single{};
  single.NumberOfDiskExtents = 1;
  single.Extents[0] = extents->Extents[0];
  return Result<VOLUME_DISK_EXTENTS>::success(single);
}

Result<std::vector<VolumeBitmapBinding>> query_bindings(
    const DiskInfo& source_disk,
    const std::span<const ExpectedVolumePartition> expected_partitions) {
  std::vector<wchar_t> volume_name(kVolumeNameCharacters, L'\0');
  const HANDLE search_handle = FindFirstVolumeW(
      volume_name.data(), static_cast<DWORD>(volume_name.size()));
  if (search_handle == INVALID_HANDLE_VALUE) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(
        clonecore::make_win32_error(
            ErrorCode::enumeration_failed,
            L"Windowsボリューム列挙の開始",
            GetLastError()));
  }
  VolumeSearch search(search_handle);
  std::vector<VolumeBitmapBinding> bindings;
  for (;;) {
    const std::wstring safe_volume_name(volume_name.data());
    std::wstring open_path = safe_volume_name;
    if (!open_path.empty() && open_path.back() == L'\\') {
      open_path.pop_back();
    }
    UniqueHandle volume(CreateFileW(
        open_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (volume) {
      const auto extent = query_single_extent(volume.get());
      if (extent &&
          extent.value().Extents[0].DiskNumber == source_disk.disk_number &&
          extent.value().Extents[0].StartingOffset.QuadPart >= 0) {
        const std::uint64_t start = static_cast<std::uint64_t>(
            extent.value().Extents[0].StartingOffset.QuadPart);
        const auto partition = std::find_if(
            expected_partitions.begin(),
            expected_partitions.end(),
            [&](const auto& candidate) {
              return candidate.offset_bytes == start;
            });
        if (partition != expected_partitions.end()) {
          const bool duplicate = std::any_of(
              bindings.begin(), bindings.end(), [&](const auto& binding) {
                return binding.partition_entry_index ==
                       partition->table_index;
              });
          if (duplicate) {
            return Result<std::vector<VolumeBitmapBinding>>::failure(
                mapping_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DUP_NAME,
                    L"NTFSボリューム対応付け",
                    L"同じパーティションへ複数のVolume GUIDが対応しました"));
          }
          bindings.push_back(VolumeBitmapBinding{
              .partition_entry_index = partition->table_index,
              .volume_device_path = safe_volume_name,
          });
        }
      }
    }

    std::fill(volume_name.begin(), volume_name.end(), L'\0');
    if (!FindNextVolumeW(
            search_handle,
            volume_name.data(),
            static_cast<DWORD>(volume_name.size()))) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_NO_MORE_FILES) {
        break;
      }
      return Result<std::vector<VolumeBitmapBinding>>::failure(
          clonecore::make_win32_error(
              ErrorCode::enumeration_failed,
              L"Windowsボリューム列挙の反復",
              native_code));
    }
  }

  for (const auto& partition : expected_partitions) {
    const bool found = std::any_of(
        bindings.begin(), bindings.end(), [&](const auto& binding) {
          return binding.partition_entry_index == partition.table_index;
        });
    if (!found) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
          ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"NTFSボリューム対応付け",
        L"対象NTFSパーティションのVolume GUIDを一意に特定できません"));
    }
  }
  return Result<std::vector<VolumeBitmapBinding>>::success(
      std::move(bindings));
}

Result<std::uint64_t> checked_partition_offset(
    const std::uint64_t first_lba,
    const std::uint32_t sector_size,
    const std::wstring_view operation) {
  if (sector_size == 0 ||
      first_lba > std::numeric_limits<std::uint64_t>::max() / sector_size) {
    return Result<std::uint64_t>::failure(mapping_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"パーティション開始位置がオーバーフローしました"));
  }
  return Result<std::uint64_t>::success(first_lba * sector_size);
}

}  // namespace

Result<std::uint32_t> query_single_disk_number_for_local_path(
    const std::wstring& candidate_path) {
  const std::filesystem::path path(candidate_path);
  const std::filesystem::path parent = path.parent_path();
  const std::wstring root_name = path.root_name().wstring();
  if (!path.is_absolute() || parent.empty() || root_name.size() != 2U ||
      root_name[1] != L':' || path.root_directory().empty()) {
    return Result<std::uint32_t>::failure(mapping_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ローカルパスの物理ディスク対応",
        L"ドライブ文字から始まる既存フォルダー内の絶対パスが必要です"));
  }

  std::filesystem::path current = parent.root_path();
  const auto verify_directory = [&](const std::filesystem::path& directory)
      -> clonecore::Status {
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Status::failure(mapping_error(
          ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                 : ERROR_REPARSE_TAG_INVALID,
          L"ローカルパスのフォルダー検証",
          L"存在する通常フォルダーだけを使用でき、reparse pointは経由できません"));
    }
    return clonecore::success_status();
  };
  auto verified = verify_directory(current);
  if (!verified) {
    return Result<std::uint32_t>::failure(verified.error());
  }
  for (const auto& component : parent.relative_path()) {
    if (component == L".") {
      continue;
    }
    if (component == L"..") {
      return Result<std::uint32_t>::failure(mapping_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          L"ローカルパスのフォルダー検証",
          L"親参照を含む保存先は使用できません"));
    }
    current /= component;
    verified = verify_directory(current);
    if (!verified) {
      return Result<std::uint32_t>::failure(verified.error());
    }
  }

  std::vector<wchar_t> volume_root(kVolumeNameCharacters, L'\0');
  if (!GetVolumePathNameW(
          parent.c_str(),
          volume_root.data(),
          static_cast<DWORD>(volume_root.size()))) {
    return Result<std::uint32_t>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"ローカルパスVolume root取得",
        GetLastError()));
  }
  std::vector<wchar_t> volume_name(kVolumeNameCharacters, L'\0');
  if (!GetVolumeNameForVolumeMountPointW(
          volume_root.data(),
          volume_name.data(),
          static_cast<DWORD>(volume_name.size()))) {
    return Result<std::uint32_t>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"ローカルパスVolume GUID取得",
        GetLastError()));
  }
  std::wstring open_path(volume_name.data());
  if (open_path.ends_with(L'\\')) {
    open_path.pop_back();
  }
  UniqueHandle volume(CreateFileW(
      open_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  DWORD volume_open_error = volume ? ERROR_SUCCESS : GetLastError();
  if (!volume && volume_open_error == ERROR_ACCESS_DENIED) {
    // A standard user can query ordinary Windows volume extents with a
    // metadata-only handle, while WinPE needs GENERIC_READ for its guarded
    // storage-number/partition-information fallback. Prefer the stronger
    // handle, but preserve the non-elevated Windows verification path when
    // the OS rejects read access.
    volume.reset(CreateFileW(
        open_path.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    volume_open_error = volume ? ERROR_SUCCESS : GetLastError();
  }
  if (!volume) {
    return Result<std::uint32_t>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"ローカルパスVolume照会",
        volume_open_error));
  }
  auto extent = query_single_extent(volume.get());
  if (!extent && extent.error().code == ErrorCode::query_failed) {
    const std::wstring dos_volume_path = L"\\\\.\\" + root_name;
    UniqueHandle dos_volume(CreateFileW(
        dos_volume_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!dos_volume) {
      return Result<std::uint32_t>::failure(clonecore::make_win32_error(
          ErrorCode::query_failed,
          L"ローカルパスDOSボリューム代替照会",
          GetLastError()));
    }
    extent = query_single_extent(dos_volume.get());
    if (!extent) {
      return Result<std::uint32_t>::failure(extent.error());
    }

    std::vector<wchar_t> rechecked_volume_name(
        kVolumeNameCharacters, L'\0');
    if (!GetVolumeNameForVolumeMountPointW(
            volume_root.data(),
            rechecked_volume_name.data(),
            static_cast<DWORD>(rechecked_volume_name.size()))) {
      return Result<std::uint32_t>::failure(clonecore::make_win32_error(
          ErrorCode::query_failed,
          L"ローカルパスVolume GUID再確認",
          GetLastError()));
    }
    if (CompareStringOrdinal(
            volume_name.data(),
            -1,
            rechecked_volume_name.data(),
            -1,
            TRUE) != CSTR_EQUAL) {
      return Result<std::uint32_t>::failure(mapping_error(
          ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"ローカルパスVolume GUID再確認",
          L"代替照会の前後でドライブ文字のVolume GUIDが変化しました"));
    }
  }
  if (!extent || extent.value().Extents[0].ExtentLength.QuadPart <= 0) {
    return extent
        ? Result<std::uint32_t>::failure(mapping_error(
              ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"ローカルパス物理範囲検証",
              L"保存先Volumeの物理範囲が不正です"))
        : Result<std::uint32_t>::failure(extent.error());
  }
  return Result<std::uint32_t>::success(
      extent.value().Extents[0].DiskNumber);
}

Result<std::vector<VolumeBitmapBinding>>
query_windows_volume_bindings_by_offset(
    const DiskInfo& source_disk,
    const std::span<const VolumePartitionLocation> expected_partitions) {
  if (source_disk.disk_number ==
          (std::numeric_limits<std::uint32_t>::max)() ||
      source_disk.size_bytes == 0U || source_disk.logical_sector_size == 0U ||
      expected_partitions.empty()) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"任意ボリューム対応付け対象",
        L"ディスク寸法または対象パーティションが不正です"));
  }
  std::vector<ExpectedVolumePartition> expected;
  expected.reserve(expected_partitions.size());
  for (std::size_t index = 0; index < expected_partitions.size(); ++index) {
    const auto& partition = expected_partitions[index];
    if (partition.offset_bytes >= source_disk.size_bytes ||
        partition.offset_bytes % source_disk.logical_sector_size != 0U) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"任意ボリューム対応付け範囲",
          L"対象パーティション開始位置がディスク境界外または非整列です"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (expected_partitions[previous].table_index == partition.table_index ||
          expected_partitions[previous].offset_bytes == partition.offset_bytes) {
        return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
            ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"任意ボリューム対応付け一意性",
            L"パーティション番号または開始位置が重複しています"));
      }
    }
    expected.push_back(ExpectedVolumePartition{
        .table_index = partition.table_index,
        .offset_bytes = partition.offset_bytes,
    });
  }
  return query_bindings(source_disk, expected);
}

Result<std::vector<VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::GptDisk& source_gpt) {
  if (source_gpt.logical_sector_size == 0 ||
      source_gpt.sector_count >
          std::numeric_limits<std::uint64_t>::max() /
              source_gpt.logical_sector_size ||
      source_disk.disk_number == std::numeric_limits<std::uint32_t>::max() ||
      source_disk.size_bytes != source_gpt.sector_count *
                                      source_gpt.logical_sector_size ||
      source_disk.logical_sector_size != source_gpt.logical_sector_size) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ボリューム対応付け対象の再確認",
        L"再列挙ディスクとGPT解析結果の寸法が一致しません"));
  }

  std::vector<ExpectedVolumePartition> expected;
  for (const auto& partition : source_gpt.partitions) {
    if (partition.type_guid != clonecore::gpt_type_basic_data()) {
      continue;
    }
    const auto offset = checked_partition_offset(
        partition.first_lba,
        source_gpt.logical_sector_size,
        L"GPTパーティション開始位置計算");
    if (!offset) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(offset.error());
    }
    expected.push_back(ExpectedVolumePartition{
        .table_index = partition.entry_index,
        .offset_bytes = offset.value(),
    });
  }
  return query_bindings(source_disk, expected);
}

Result<std::vector<VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::MbrDisk& source_mbr) {
  if (source_mbr.logical_sector_size == 0 ||
      source_mbr.sector_count >
          std::numeric_limits<std::uint64_t>::max() /
              source_mbr.logical_sector_size ||
      source_disk.disk_number == std::numeric_limits<std::uint32_t>::max() ||
      source_disk.size_bytes != source_mbr.sector_count *
                                      source_mbr.logical_sector_size ||
      source_disk.logical_sector_size != source_mbr.logical_sector_size) {
    return Result<std::vector<VolumeBitmapBinding>>::failure(mapping_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ボリューム対応付け対象の再確認",
        L"再列挙ディスクとMBR解析結果の寸法が一致しません"));
  }

  std::vector<ExpectedVolumePartition> expected;
  for (const auto& partition : source_mbr.partitions) {
    const bool snapshot_or_locked_basic = partition.type == 0x07;
    const bool locked_non_system_fat32 =
        (partition.type == 0x0B || partition.type == 0x0C) &&
        !partition.active;
    if (!snapshot_or_locked_basic && !locked_non_system_fat32) {
      continue;
    }
    const auto offset = checked_partition_offset(
        partition.first_lba,
        source_mbr.logical_sector_size,
        L"MBRパーティション開始位置計算");
    if (!offset) {
      return Result<std::vector<VolumeBitmapBinding>>::failure(offset.error());
    }
    expected.push_back(ExpectedVolumePartition{
        .table_index = partition.table_index,
        .offset_bytes = offset.value(),
    });
  }
  return query_bindings(source_disk, expected);
}

}  // namespace ytec::diskmodel

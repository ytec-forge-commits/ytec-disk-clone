#include "ytec/windowsapp/online_direct_clone.h"

#include "ytec/bootrepair/clone_boot_finalization.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/offline_mbr_clone.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace ytec::windowsapp {
namespace {

bool all_zero(const imageformat::Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

clonecore::Error direct_error(
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

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0 &&
      right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
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

bool is_canonical_volume_guid_path(std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (!path.starts_with(prefix) || !path.ends_with(L'\\')) {
    return false;
  }
  path.remove_suffix(1U);
  if (!path.ends_with(L'}') || path.size() != prefix.size() + 37U) {
    return false;
  }
  const auto body = path.substr(prefix.size(), 36U);
  for (std::size_t index = 0U; index < body.size(); ++index) {
    const bool hyphen = index == 8U || index == 13U || index == 18U ||
        index == 23U;
    if ((hyphen && body[index] != L'-') ||
        (!hyphen && !is_hex(body[index]))) {
      return false;
    }
  }
  return true;
}

std::wstring trim_volume_root_slash(std::wstring path) {
  if (!path.empty() && path.back() == L'\\') {
    path.pop_back();
  }
  return path;
}

struct ExactVolumeExtent final {
  std::uint32_t disk_number{};
  std::uint64_t offset{};
  std::uint64_t length{};
};

clonecore::Result<ExactVolumeExtent> query_exact_volume_extent(
    const HANDLE volume) {
  std::vector<std::byte> buffer(64U * 1024U);
  DWORD returned = 0U;
  if (DeviceIoControl(
          volume,
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0U,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &returned,
          nullptr) == FALSE) {
    return clonecore::Result<ExactVolumeExtent>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"オンラインFAT/exFAT Volume extent照会",
            GetLastError()));
  }
  constexpr std::size_t header = offsetof(VOLUME_DISK_EXTENTS, Extents);
  if (returned < header + sizeof(DISK_EXTENT)) {
    return clonecore::Result<ExactVolumeExtent>::failure(direct_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オンラインFAT/exFAT Volume extent応答",
        L"Volume extent応答が固定headerより短いです"));
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents != 1U ||
      extents->Extents[0].StartingOffset.QuadPart < 0 ||
      extents->Extents[0].ExtentLength.QuadPart <= 0) {
    return clonecore::Result<ExactVolumeExtent>::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンラインFAT/exFAT Volume extent一意性",
        L"Volumeを単一の正の物理範囲へ拘束できません"));
  }
  return clonecore::Result<ExactVolumeExtent>::success({
      .disk_number = extents->Extents[0].DiskNumber,
      .offset = static_cast<std::uint64_t>(
          extents->Extents[0].StartingOffset.QuadPart),
      .length = static_cast<std::uint64_t>(
          extents->Extents[0].ExtentLength.QuadPart),
  });
}

clonecore::Result<clonecore::ByteRange> make_partition_range(
    const std::uint64_t first_lba,
    const std::uint64_t sector_count,
    const std::uint32_t sector_size,
    const std::uint64_t disk_size,
    const std::wstring_view operation) {
  std::uint64_t offset{};
  std::uint64_t length{};
  std::uint64_t end{};
  if (sector_count == 0 ||
      !checked_multiply(first_lba, sector_size, offset) ||
      !checked_multiply(sector_count, sector_size, length) ||
      !checked_add(offset, length, end) || end > disk_size) {
    return clonecore::Result<clonecore::ByteRange>::failure(direct_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"パーティション位置がオーバーフローまたはディスク境界外です"));
  }
  return clonecore::Result<clonecore::ByteRange>::success(
      clonecore::ByteRange{.offset = offset, .length = length});
}

clonecore::Status validate_boot_read(
    const clonecore::ISourceDiskReader& source,
    const clonecore::ByteRange& range,
    const bool ntfs) {
  const auto boot = source.read(
      range.offset, source.logical_sector_size());
  if (!boot) {
    return clonecore::Status::failure(boot.error());
  }
  if (boot.value().size() != source.logical_sector_size()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::io_failed,
        ERROR_HANDLE_EOF,
        L"オンライン直接クローン ブートセクター読取り",
        L"コピー元から論理セクターを完全に読み取れませんでした"));
  }
  if (ntfs) {
    const auto geometry = clonecore::parse_ntfs_geometry(
        boot.value(), source.logical_sector_size(), range.length);
    return geometry ? clonecore::success_status()
                    : clonecore::Status::failure(geometry.error());
  }
  return clonecore::validate_fat32_boot_sector(
      boot.value(), source.logical_sector_size(), range.length);
}

clonecore::Result<clonecore::BasicDataFileSystem>
classify_basic_file_system_read(
    const clonecore::ISourceDiskReader& source,
    const clonecore::ByteRange& range) {
  const auto boot = source.read(range.offset, source.logical_sector_size());
  if (!boot) {
    return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
        boot.error());
  }
  if (boot.value().size() != source.logical_sector_size()) {
    return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
        direct_error(
            clonecore::ErrorCode::io_failed,
            ERROR_HANDLE_EOF,
            L"オンライン基本データ ブートセクター読取り",
            L"コピー元から論理セクターを完全に読み取れませんでした"));
  }
  const auto identified = clonecore::classify_basic_data_file_system(
      boot.value(), source.logical_sector_size(), range.length);
  if (!identified ||
      identified.value() == clonecore::BasicDataFileSystem::ntfs) {
    return identified;
  }

  std::uint64_t declared_volume_bytes{};
  if (identified.value() == clonecore::BasicDataFileSystem::fat32) {
    const auto geometry = clonecore::parse_fat32_geometry(
        boot.value(), source.logical_sector_size(), range.length);
    if (!geometry) {
      return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
          geometry.error());
    }
    if (!checked_multiply(
            geometry.value().total_sectors,
            geometry.value().bytes_per_sector,
            declared_volume_bytes)) {
      return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
          direct_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"オンラインFAT32 Volume境界",
              L"FAT32宣言容量を安全に計算できません"));
    }
  } else {
    const auto geometry = clonecore::parse_exfat_geometry(
        boot.value(), source.logical_sector_size(), range.length);
    if (!geometry) {
      return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
          geometry.error());
    }
    if (!checked_multiply(
            geometry.value().total_sectors,
            geometry.value().bytes_per_sector,
            declared_volume_bytes)) {
      return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
          direct_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"オンラインexFAT Volume境界",
              L"exFAT宣言容量を安全に計算できません"));
    }
  }
  if (declared_volume_bytes != range.length) {
    return clonecore::Result<clonecore::BasicDataFileSystem>::failure(
        direct_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"オンラインFAT/exFAT Volume境界",
            L"排他lockで固定できるVolume宣言範囲がパーティション全体と一致しません"));
  }
  return identified;
}

clonecore::Result<std::size_t> find_binding(
    const std::uint32_t partition_index,
    const std::span<const clonecore::VolumeBitmapBinding> bindings,
    const std::span<const std::uint8_t> used) {
  std::size_t found = bindings.size();
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (bindings[index].partition_entry_index != partition_index) {
      continue;
    }
    if (found != bindings.size() || used[index] != 0 ||
        bindings[index].volume_device_path.empty()) {
      return clonecore::Result<std::size_t>::failure(direct_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"オンライン直接クローン Volume対応",
          L"NTFSパーティションへのVolume対応が空または重複しています"));
    }
    found = index;
  }
  if (found == bindings.size()) {
    return clonecore::Result<std::size_t>::failure(direct_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"オンライン直接クローン Volume対応",
        L"NTFSパーティションに対応するVolume GUIDがありません"));
  }
  return clonecore::Result<std::size_t>::success(found);
}

clonecore::Status reject_unused_or_duplicate_bindings(
    const std::span<const clonecore::VolumeBitmapBinding> bindings,
    const std::span<const std::uint8_t> used) {
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (used[index] == 0) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン Volume対応件数",
          L"パーティション計画にないVolume対応が含まれています"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (equals_case_insensitive(
              bindings[index].volume_device_path,
              bindings[previous].volume_device_path)) {
        return clonecore::Status::failure(direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"オンライン直接クローン Volume重複",
            L"複数パーティションが同じVolume GUIDを示しています"));
      }
    }
  }
  return clonecore::success_status();
}

bool range_less(
    const clonecore::ByteRange& left,
    const clonecore::ByteRange& right) noexcept {
  return left.offset < right.offset;
}

clonecore::Status validate_layout_ranges(OnlineDirectSourceLayout& layout) {
  if (layout.snapshot_partitions.empty() && layout.locked_partitions.empty()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン 整合性対象",
        L"VSS Snapshotまたは排他Volume lockで固定できるパーティションがありません"));
  }
  std::sort(
      layout.snapshot_partitions.begin(),
      layout.snapshot_partitions.end(),
      [](const auto& left, const auto& right) {
        return left.disk_offset < right.disk_offset;
      });
  std::sort(
      layout.locked_partitions.begin(),
      layout.locked_partitions.end(),
      [](const auto& left, const auto& right) {
        return left.disk_offset < right.disk_offset;
      });
  std::sort(
      layout.static_physical_ranges.begin(),
      layout.static_physical_ranges.end(),
      range_less);
  std::uint64_t previous_end{};
  for (std::size_t index = 0;
       index < layout.snapshot_partitions.size(); ++index) {
    const auto& route = layout.snapshot_partitions[index];
    std::uint64_t end{};
    if (route.length == 0 || route.volume_guid_path.empty() ||
        !checked_add(route.disk_offset, route.length, end) ||
        (index != 0 && route.disk_offset < previous_end)) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン Snapshot範囲",
          L"Snapshot対象範囲が空、重複、またはオーバーフローしています"));
    }
    previous_end = end;
  }
  previous_end = 0;
  for (std::size_t index = 0;
       index < layout.locked_partitions.size(); ++index) {
    const auto& route = layout.locked_partitions[index];
    std::uint64_t end{};
    if (route.length == 0 || route.volume_guid_path.empty() ||
        !checked_add(route.disk_offset, route.length, end) ||
        (index != 0 && route.disk_offset < previous_end)) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン Volume lock範囲",
          L"Volume lock対象範囲が空、重複、またはオーバーフローしています"));
    }
    previous_end = end;
  }
  previous_end = 0;
  for (std::size_t index = 0;
       index < layout.static_physical_ranges.size(); ++index) {
    const auto& range = layout.static_physical_ranges[index];
    std::uint64_t end{};
    if (range.length == 0 || !checked_add(range.offset, range.length, end) ||
        (index != 0 && range.offset < previous_end)) {
      return clonecore::Status::failure(direct_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オンライン直接クローン 固定領域",
          L"固定領域が空、重複、またはオーバーフローしています"));
    }
    previous_end = end;
  }
  for (const auto& snapshot : layout.snapshot_partitions) {
    const std::uint64_t snapshot_end =
        snapshot.disk_offset + snapshot.length;
    for (const auto& fixed : layout.static_physical_ranges) {
      const std::uint64_t fixed_end = fixed.offset + fixed.length;
      if (snapshot.disk_offset < fixed_end && fixed.offset < snapshot_end) {
        return clonecore::Status::failure(direct_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"オンライン直接クローン 読取り経路分離",
            L"Snapshot領域と物理固定領域が重複しています"));
      }
    }
    for (const auto& locked : layout.locked_partitions) {
      const std::uint64_t locked_end = locked.disk_offset + locked.length;
      if (snapshot.disk_offset < locked_end &&
          locked.disk_offset < snapshot_end) {
        return clonecore::Status::failure(direct_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"オンライン直接クローン 読取り経路分離",
            L"Snapshot領域とVolume lock領域が重複しています"));
      }
    }
  }
  for (const auto& locked : layout.locked_partitions) {
    const std::uint64_t locked_end = locked.disk_offset + locked.length;
    for (const auto& fixed : layout.static_physical_ranges) {
      const std::uint64_t fixed_end = fixed.offset + fixed.length;
      if (locked.disk_offset < fixed_end && fixed.offset < locked_end) {
        return clonecore::Status::failure(direct_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"オンライン直接クローン 読取り経路分離",
            L"Volume lock領域と物理固定領域が重複しています"));
      }
    }
  }
  return clonecore::success_status();
}

bool same_range(
    const clonecore::ByteRange& left,
    const clonecore::ByteRange& right) noexcept {
  return left.offset == right.offset && left.length == right.length;
}

bool same_source_layout(
    const OnlineDirectSourceLayout& left,
    const OnlineDirectSourceLayout& right) {
  if (left.partition_style != right.partition_style ||
      left.snapshot_partitions.size() != right.snapshot_partitions.size() ||
      left.locked_partitions.size() != right.locked_partitions.size() ||
      left.static_physical_ranges.size() !=
          right.static_physical_ranges.size()) {
    return false;
  }
  for (std::size_t index = 0;
       index < left.locked_partitions.size(); ++index) {
    const auto& lhs = left.locked_partitions[index];
    const auto& rhs = right.locked_partitions[index];
    if (lhs.partition_index != rhs.partition_index ||
        lhs.disk_offset != rhs.disk_offset || lhs.length != rhs.length ||
        lhs.file_system != rhs.file_system ||
        !equals_case_insensitive(
            lhs.volume_guid_path, rhs.volume_guid_path)) {
      return false;
    }
  }
  for (std::size_t index = 0;
       index < left.snapshot_partitions.size(); ++index) {
    const auto& lhs = left.snapshot_partitions[index];
    const auto& rhs = right.snapshot_partitions[index];
    if (lhs.partition_index != rhs.partition_index ||
        lhs.disk_offset != rhs.disk_offset || lhs.length != rhs.length ||
        !equals_case_insensitive(
            lhs.volume_guid_path, rhs.volume_guid_path)) {
      return false;
    }
  }
  for (std::size_t index = 0;
       index < left.static_physical_ranges.size(); ++index) {
    if (!same_range(
            left.static_physical_ranges[index],
            right.static_physical_ranges[index])) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_observation(
    const diskmodel::ReidentifiedPhysicalClone& observed) {
  if (!observed.source.offline.has_value() ||
      !observed.source.read_only.has_value() ||
      !observed.source.removable.has_value() ||
      !observed.target.offline.has_value() ||
      !observed.target.read_only.has_value() ||
      !observed.target.removable.has_value()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"オンライン直接クローン ディスク属性",
        L"コピー元またはコピー先の安全属性を確定できません"));
  }
  if (observed.source.offline.value() ||
      observed.source.removable.value()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン コピー元状態",
        L"オンラインの固定基本ディスクだけをコピー元にできます"));
  }
  if (observed.target.read_only.value() ||
      observed.target.removable.value()) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::access_denied,
        observed.target.read_only.value()
            ? ERROR_WRITE_PROTECT
            : ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン コピー先属性",
        L"読取り専用ディスクまたはremovable媒体はコピー先にできません"));
  }
  if (diskmodel::disk_health_operation_advice(
          observed.target.health, false) ==
      diskmodel::DiskHealthOperationAdvice::block_target) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_DEVICE_HARDWARE_ERROR,
        L"オンライン直接クローン コピー先健康状態",
        L"SMARTまたはNVMeが注意・異常を報告しているディスクはコピー先にできません"));
  }
  const auto style = diskmodel::normalize_disk_partition_style(
      observed.target.partition_style,
      observed.target.partitions.size());
  if (style != diskmodel::PartitionStyle::raw &&
      style != diskmodel::PartitionStyle::gpt &&
      style != diskmodel::PartitionStyle::mbr) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン直接クローン コピー先形式",
        L"RAW、GPT、MBRとして確定できないコピー先は消去できません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_reviewed_layouts(
    const OnlineDirectCloneRequest& request,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const std::wstring_view phase) {
  auto source_hash = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.source);
  auto target_hash = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.target);
  if (!source_hash || !target_hash) {
    return clonecore::Status::failure(
        !source_hash ? source_hash.error() : target_hash.error());
  }
  if (source_hash.value() != request.expected_source_layout_hash ||
      target_hash.value() != request.expected_target_layout_hash) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        std::wstring(L"オンライン直接クローン ") +
            std::wstring(phase) + L"レイアウト照合",
        L"最終確認画面の後にコピー元またはコピー先のパーティション形式・配置が変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_dependencies(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  if (!request.administrator) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"オンライン直接クローン 管理者確認",
        L"アプリ起動時の管理者権限が必要です"));
  }
  if (all_zero(request.expected_source_layout_hash) ||
      all_zero(request.expected_target_layout_hash)) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"オンライン直接クローン レビューHash",
        L"最終確認したコピー元・コピー先レイアウトHashがありません"));
  }
  if (request.maximum_chunk_bytes == 0 ||
      request.maximum_chunk_bytes > 16U * 1024U * 1024U) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンライン直接クローン 要求",
        L"コピー元または転送チャンク寸法が不正です"));
  }
  if (!dependencies.reidentify_clone ||
      !dependencies.open_read_only_source ||
      !dependencies.query_gpt_bindings ||
      !dependencies.query_mbr_bindings ||
      !dependencies.run_snapshot_workflow ||
      !dependencies.open_snapshot_reader ||
      !dependencies.open_locked_volume ||
      !dependencies.make_snapshot_bitmap_provider ||
      !dependencies.set_clone_target_offline ||
      !dependencies.set_physical_target_offline ||
      !dependencies.open_offline_target ||
      !dependencies.collect_mbr_signatures ||
      !dependencies.execute_clone_engine ||
      !dependencies.finalize_target_boot) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンライン直接クローン 依存境界",
        L"再識別、VSS/Volume lock Reader、コピー先、またはClone Engineがありません"));
  }
  return clonecore::success_status();
}

class LockedVolumeReader final : public clonecore::ISourceDiskReader {
 public:
  LockedVolumeReader(
      clonecore::UniqueHandle handle,
      const std::uint64_t size,
      const std::uint32_t sector_size)
      : handle_(std::move(handle)), size_(size), sector_size_(sector_size) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return sector_size_;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::uint64_t end{};
    if (!handle_ || length == 0U || length > MAXDWORD ||
        offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()) ||
        !checked_add(offset, length, end) || end > size_) {
      return clonecore::Result<std::vector<std::byte>>::failure(direct_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"オンラインFAT/exFAT lock Reader範囲",
          L"読取り要求が空、過大、オーバーフロー、またはVolume境界外です"));
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN) == FALSE) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"オンラインFAT/exFAT lock Reader seek",
              GetLastError()));
    }
    std::vector<std::byte> bytes(length);
    DWORD received = 0U;
    if (ReadFile(
            handle_.get(),
            bytes.data(),
            static_cast<DWORD>(length),
            &received,
            nullptr) == FALSE || received != length) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<std::vector<std::byte>>::failure(direct_error(
          clonecore::ErrorCode::io_failed,
          native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code,
          L"オンラインFAT/exFAT lock Reader読取り",
          L"排他lockしたVolumeから要求長を完全に読み取れませんでした"));
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

 private:
  mutable clonecore::UniqueHandle handle_;
  std::uint64_t size_{};
  std::uint32_t sector_size_{};
};

class RejectingNtfsUsedRangeProvider final
    : public clonecore::INtfsUsedRangeProvider {
 public:
  [[nodiscard]] clonecore::Result<std::vector<clonecore::ByteRange>>
  query_used_ranges(
      const std::uint32_t,
      const clonecore::NtfsGeometry&) override {
    return clonecore::Result<std::vector<clonecore::ByteRange>>::failure(
        direct_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"オンラインFAT/exFAT NTFS Bitmap",
            L"Snapshotのない経路でNTFS Bitmapが要求されました"));
  }
};

class OnlineDirectCompositeReader final
    : public clonecore::ISourceDiskReader {
 public:
  OnlineDirectCompositeReader(
      const clonecore::ISourceDiskReader* physical,
      std::vector<OnlineDirectSnapshotReader> snapshots,
      std::vector<OnlineDirectLockedReader> locked,
      std::vector<clonecore::ByteRange> fixed)
      : physical_(physical),
        snapshots_(std::move(snapshots)),
        locked_(std::move(locked)),
        fixed_(std::move(fixed)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return physical_->size_bytes();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return physical_->logical_sector_size();
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::uint64_t end{};
    if (length == 0 || !checked_add(offset, length, end) ||
        end > size_bytes()) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          direct_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"オンラインSnapshot合成Reader範囲",
              L"読取り要求が空、オーバーフロー、またはディスク境界外です"));
    }
    for (const auto& snapshot : snapshots_) {
      const std::uint64_t route_end =
          snapshot.disk_offset + snapshot.length;
      if (offset >= snapshot.disk_offset && end <= route_end) {
        auto bytes = snapshot.reader->read(
            offset - snapshot.disk_offset, length);
        if (!bytes || bytes.value().size() != length) {
          return bytes
              ? clonecore::Result<std::vector<std::byte>>::failure(
                    direct_error(
                        clonecore::ErrorCode::io_failed,
                        ERROR_HANDLE_EOF,
                        L"オンラインSnapshot合成Reader",
                        L"Snapshotから要求長を完全に読み取れませんでした"))
              : clonecore::Result<std::vector<std::byte>>::failure(
                    bytes.error());
        }
        return bytes;
      }
      if (offset < route_end && snapshot.disk_offset < end) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインSnapshot合成Reader境界",
                L"1回の読取りがSnapshot境界をまたいでいます"));
      }
    }
    for (const auto& locked : locked_) {
      const std::uint64_t route_end = locked.disk_offset + locked.length;
      if (offset >= locked.disk_offset && end <= route_end) {
        auto bytes = locked.reader->read(
            offset - locked.disk_offset, length);
        if (!bytes || bytes.value().size() != length) {
          return bytes
              ? clonecore::Result<std::vector<std::byte>>::failure(
                    direct_error(
                        clonecore::ErrorCode::io_failed,
                        ERROR_HANDLE_EOF,
                        L"オンラインVolume lock合成Reader",
                        L"排他lock Volumeから要求長を完全に読み取れませんでした"))
              : clonecore::Result<std::vector<std::byte>>::failure(
                    bytes.error());
        }
        return bytes;
      }
      if (offset < route_end && locked.disk_offset < end) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインVolume lock合成Reader境界",
                L"1回の読取りがVolume lock境界をまたいでいます"));
      }
    }
    for (const auto& fixed : fixed_) {
      const std::uint64_t route_end = fixed.offset + fixed.length;
      if (offset >= fixed.offset && end <= route_end) {
        auto bytes = physical_->read(offset, length);
        if (!bytes || bytes.value().size() != length) {
          return bytes
              ? clonecore::Result<std::vector<std::byte>>::failure(
                    direct_error(
                        clonecore::ErrorCode::io_failed,
                        ERROR_HANDLE_EOF,
                        L"オンライン固定領域Reader",
                        L"物理ディスクから要求長を完全に読み取れませんでした"))
              : clonecore::Result<std::vector<std::byte>>::failure(
                    bytes.error());
        }
        return bytes;
      }
    }
    return clonecore::Result<std::vector<std::byte>>::failure(direct_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンラインSnapshot合成Reader経路",
        L"Snapshotまたは検証済み固定領域に含まれない読取りを拒否しました"));
  }

 private:
  const clonecore::ISourceDiskReader* physical_{};
  std::vector<OnlineDirectSnapshotReader> snapshots_;
  std::vector<OnlineDirectLockedReader> locked_;
  std::vector<clonecore::ByteRange> fixed_;
};

clonecore::Error append_offline_failure(
    clonecore::Error primary,
    const clonecore::Status& offline) {
  if (!offline) {
    primary.message +=
        L"。コピー先offline状態の再確認にも失敗しました: " +
        offline.error().operation;
  }
  return primary;
}

clonecore::Status reprotect_target_offline_after_exception(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  try {
    return dependencies.set_physical_target_offline(
        request.expected_target, request.confirmation, true);
  } catch (...) {
    return clonecore::Status::failure(direct_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"オンライン直接クローン 例外後offline保護",
        L"例外後のコピー先offline化でも例外が発生し、状態を確認できません"));
  }
}

clonecore::Result<std::vector<std::uint32_t>>
collect_connected_mbr_signatures_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::MbrDisk& source_mbr) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  const auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        direct_error(
            clonecore::ErrorCode::query_failed,
            ERROR_INVALID_DATA,
            L"オンラインMBR署名 全ディスク列挙",
            L"未解決の列挙診断があるため署名衝突を確認できません"));
  }
  std::vector<std::uint32_t> signatures{source_mbr.disk_signature};
  for (const auto& disk : report.value().disks) {
    if (diskmodel::normalize_disk_partition_style(
            disk.partition_style, disk.partitions.size()) !=
        diskmodel::PartitionStyle::mbr) {
      continue;
    }
    const auto identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          identity.error());
    }
    if (clonecore::validate_stable_identity(
            expected_source, identity.value(), L"オンラインMBRコピー元")) {
      continue;
    }
    auto handle =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            identity.value());
    if (!handle) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          handle.error());
    }
    const auto mbr = clonecore::parse_mbr(*handle.value().reader);
    if (!mbr) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          mbr.error());
    }
    signatures.push_back(mbr.value().disk_signature);
  }
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(
      std::unique(signatures.begin(), signatures.end()),
      signatures.end());
  return clonecore::Result<std::vector<std::uint32_t>>::success(
      std::move(signatures));
}

clonecore::Result<OnlineDirectCloneEngineReport>
execute_clone_engine_with_native_core(
    const OnlineDirectCloneEngineContext& context) {
  if (context.request == nullptr || context.observed_source == nullptr ||
      context.observed_target == nullptr || context.source == nullptr ||
      context.target == nullptr ||
      context.snapshot_bitmap_provider == nullptr) {
    return clonecore::Result<OnlineDirectCloneEngineReport>::failure(
        direct_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"オンラインClone Engine接続",
            L"要求、識別情報、Reader、Writer、またはBitmap Providerがありません"));
  }
  if (context.partition_style ==
      OnlineDirectClonePartitionStyle::gpt) {
    auto guid_generator = clonecore::make_windows_guid_generator();
    const auto report = clonecore::execute_offline_gpt_clone(
        clonecore::OfflineGptCloneRequest{
            .expected_source = context.request->expected_source,
            .observed_source = *context.observed_source,
            .expected_target = context.request->expected_target,
            .observed_target = *context.observed_target,
            .confirmation = context.request->confirmation,
            .maximum_chunk_bytes =
                context.request->maximum_chunk_bytes,
            .callbacks = context.request->callbacks,
        },
        *context.source,
        *context.target,
        *context.snapshot_bitmap_provider,
        *guid_generator);
    if (!report) {
      return clonecore::Result<OnlineDirectCloneEngineReport>::failure(
          report.error());
    }
    return clonecore::Result<OnlineDirectCloneEngineReport>::success(
        OnlineDirectCloneEngineReport{
            .copied_data_bytes = report.value().copied_data_bytes,
            .copied_partition_count =
                report.value().copied_partition_count,
            .recreated_partition_count =
                report.value().recreated_partition_count,
            .verified_write_digest =
                report.value().verified_write_digest,
            .read_back_verified = report.value().read_back_verified,
            .partition_table_committed =
                report.value().primary_gpt_committed,
        });
  }

  auto signature_generator =
      clonecore::make_windows_mbr_signature_generator();
  const auto report = clonecore::execute_offline_mbr_clone(
      clonecore::OfflineMbrCloneRequest{
          .expected_source = context.request->expected_source,
          .observed_source = *context.observed_source,
          .expected_target = context.request->expected_target,
          .observed_target = *context.observed_target,
          .confirmation = context.request->confirmation,
          .maximum_chunk_bytes =
              context.request->maximum_chunk_bytes,
          .connected_mbr_signatures =
              std::vector<std::uint32_t>(
                  context.connected_mbr_signatures.begin(),
                  context.connected_mbr_signatures.end()),
          .callbacks = context.request->callbacks,
      },
      *context.source,
      *context.target,
      *context.snapshot_bitmap_provider,
      *signature_generator);
  if (!report) {
    return clonecore::Result<OnlineDirectCloneEngineReport>::failure(
        report.error());
  }
  return clonecore::Result<OnlineDirectCloneEngineReport>::success(
      OnlineDirectCloneEngineReport{
          .copied_data_bytes = report.value().copied_data_bytes,
          .copied_partition_count =
              report.value().copied_partition_count,
          .recreated_partition_count = 0,
          .verified_write_digest =
              report.value().verified_write_digest,
          .read_back_verified = report.value().read_back_verified,
          .partition_table_committed =
              report.value().target_mbr_committed,
      });
}

}  // namespace

clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
open_locked_file_system_volume_with_windows_apis(
    const OnlineDirectLockedVolumeOpenRequest& request) {
  std::uint64_t end{};
  if (!is_canonical_volume_guid_path(request.volume_guid_path) ||
      request.length == 0U || request.logical_sector_size == 0U ||
      request.disk_offset % request.logical_sector_size != 0U ||
      request.length % request.logical_sector_size != 0U ||
      !checked_add(request.disk_offset, request.length, end)) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"オンラインFAT/exFAT Volume lock要求",
            L"canonical Volume GUID、範囲、または論理セクターが不正です"));
  }

  clonecore::UniqueHandle volume(CreateFileW(
      trim_volume_root_slash(request.volume_guid_path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            L"オンラインFAT/exFAT Volume読取り専用open",
            GetLastError()));
  }

  auto extent = query_exact_volume_extent(volume.get());
  if (!extent) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        extent.error());
  }
  if (extent.value().disk_number != request.physical_disk_number ||
      extent.value().offset != request.disk_offset ||
      extent.value().length != request.length) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"オンラインFAT/exFAT Volume extent照合",
            L"Volumeのdisk、offset、またはlengthが解析済みパーティションと一致しません"));
  }

  DWORD returned = 0U;
  if (DeviceIoControl(
          volume.get(),
          FSCTL_LOCK_VOLUME,
          nullptr,
          0U,
          nullptr,
          0U,
          &returned,
          nullptr) == FALSE) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::access_denied,
            L"オンラインFAT/exFAT 排他Volume lock",
            GetLastError()));
  }

  extent = query_exact_volume_extent(volume.get());
  if (!extent || extent.value().disk_number != request.physical_disk_number ||
      extent.value().offset != request.disk_offset ||
      extent.value().length != request.length) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        extent
            ? direct_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_REINITIALIZATION_NEEDED,
                  L"オンラインFAT/exFAT lock後extent再照合",
                  L"排他lock後にVolumeの物理範囲が変化しました")
            : extent.error());
  }

  DISK_GEOMETRY_EX geometry{};
  const BOOL geometry_queried = DeviceIoControl(
          volume.get(),
          IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
          nullptr,
          0U,
          &geometry,
          static_cast<DWORD>(sizeof(geometry)),
          &returned,
          nullptr);
  const DWORD geometry_error =
      geometry_queried == FALSE ? GetLastError() : ERROR_SUCCESS;
  if (geometry_queried == FALSE ||
      geometry.Geometry.BytesPerSector != request.logical_sector_size) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::identity_mismatch,
            geometry_error == ERROR_SUCCESS
                ? ERROR_INVALID_DATA
                : geometry_error,
            L"オンラインFAT/exFAT 論理セクター再照合",
            L"排他lock Volumeの論理セクター寸法がコピー計画と一致しません"));
  }

  std::array<wchar_t, 32U> file_system{};
  if (GetVolumeInformationByHandleW(
          volume.get(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size())) == FALSE) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"オンラインFAT/exFAT ファイルシステム再照合",
            GetLastError()));
  }
  const std::wstring_view expected_name =
      request.expected_file_system == OnlineDirectLockedFileSystem::fat32
      ? L"FAT32"
      : L"exFAT";
  if (!equals_case_insensitive(file_system.data(), expected_name)) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"オンラインFAT/exFAT ファイルシステム再照合",
            L"Volume APIが返したファイルシステムが解析済み形式と一致しません"));
  }

  auto locked_reader = std::make_unique<LockedVolumeReader>(
      std::move(volume), request.length, request.logical_sector_size);
  const auto boot = locked_reader->read(0U, request.logical_sector_size);
  if (!boot) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        boot.error());
  }
  const auto identified = clonecore::classify_basic_data_file_system(
      boot.value(), request.logical_sector_size, request.length);
  const auto expected_type =
      request.expected_file_system == OnlineDirectLockedFileSystem::fat32
      ? clonecore::BasicDataFileSystem::fat32
      : clonecore::BasicDataFileSystem::exfat;
  if (!identified || identified.value() != expected_type) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        identified
            ? direct_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_REINITIALIZATION_NEEDED,
                  L"オンラインFAT/exFAT boot signature再照合",
                  L"排他lock handleのboot signatureが解析済み形式と一致しません")
            : identified.error());
  }

  std::uint64_t declared_volume_bytes{};
  if (expected_type == clonecore::BasicDataFileSystem::fat32) {
    const auto file_system_geometry = clonecore::parse_fat32_geometry(
        boot.value(), request.logical_sector_size, request.length);
    if (!file_system_geometry ||
        !checked_multiply(
            file_system_geometry.value().total_sectors,
            file_system_geometry.value().bytes_per_sector,
            declared_volume_bytes)) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          file_system_geometry
              ? direct_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_ARITHMETIC_OVERFLOW,
                    L"オンラインFAT32 lock後Volume境界",
                    L"FAT32宣言容量を安全に計算できません")
              : file_system_geometry.error());
    }
  } else {
    const auto file_system_geometry = clonecore::parse_exfat_geometry(
        boot.value(), request.logical_sector_size, request.length);
    if (!file_system_geometry ||
        !checked_multiply(
            file_system_geometry.value().total_sectors,
            file_system_geometry.value().bytes_per_sector,
            declared_volume_bytes)) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          file_system_geometry
              ? direct_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_ARITHMETIC_OVERFLOW,
                    L"オンラインexFAT lock後Volume境界",
                    L"exFAT宣言容量を安全に計算できません")
              : file_system_geometry.error());
    }
  }
  if (declared_volume_bytes != request.length) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"オンラインFAT/exFAT lock後Volume境界",
            L"排他lock handleのVolume宣言範囲がパーティション全体と一致しません"));
  }

  std::unique_ptr<clonecore::ISourceDiskReader> result =
      std::move(locked_reader);
  return clonecore::Result<
      std::unique_ptr<clonecore::ISourceDiskReader>>::success(
      std::move(result));
}

clonecore::Result<OnlineDirectSourceLayout>
build_online_direct_source_layout(
    const diskmodel::DiskInfo& observed_source,
    const clonecore::ISourceDiskReader& read_only_source,
    const std::span<const clonecore::VolumeBitmapBinding> volume_bindings) {
  if (observed_source.size_bytes != read_only_source.size_bytes() ||
      observed_source.logical_sector_size !=
          read_only_source.logical_sector_size() ||
      observed_source.size_bytes == 0 || volume_bindings.empty()) {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        direct_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"オンライン直接クローン コピー元寸法",
        L"ディスク、Reader寸法、またはVolume対応が一致しません"));
  }

  OnlineDirectSourceLayout layout;
  std::vector<std::uint8_t> used(volume_bindings.size(), 0);
  if (observed_source.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    const auto gpt = clonecore::parse_gpt(read_only_source);
    if (!gpt) {
      return clonecore::Result<OnlineDirectSourceLayout>::failure(
          gpt.error());
    }
    layout.partition_style = OnlineDirectClonePartitionStyle::gpt;
    const auto leading = make_partition_range(
        0,
        gpt.value().first_usable_lba,
        gpt.value().logical_sector_size,
        read_only_source.size_bytes(),
        L"オンラインGPT先頭メタデータ");
    const auto trailing = make_partition_range(
        gpt.value().last_usable_lba + 1,
        gpt.value().sector_count - gpt.value().last_usable_lba - 1,
        gpt.value().logical_sector_size,
        read_only_source.size_bytes(),
        L"オンラインGPT末尾メタデータ");
    if (!leading || !trailing) {
      return clonecore::Result<OnlineDirectSourceLayout>::failure(
          leading ? trailing.error() : leading.error());
    }
    layout.static_physical_ranges.push_back(leading.value());
    layout.static_physical_ranges.push_back(trailing.value());

    for (const auto& partition : gpt.value().partitions) {
      const auto range = make_partition_range(
          partition.first_lba,
          partition.last_lba - partition.first_lba + 1,
          gpt.value().logical_sector_size,
          read_only_source.size_bytes(),
          L"オンラインGPTパーティション");
      if (!range) {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            range.error());
      }
      if (partition.type_guid == clonecore::gpt_type_basic_data()) {
        const auto file_system = classify_basic_file_system_read(
            read_only_source, range.value());
        if (!file_system) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              file_system.error());
        }
        const auto binding = find_binding(
            partition.entry_index, volume_bindings, used);
        if (!binding) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              binding.error());
        }
        used[binding.value()] = 1;
        if (file_system.value() ==
            clonecore::BasicDataFileSystem::ntfs) {
          layout.snapshot_partitions.push_back(
              OnlineDirectSnapshotPartition{
                .partition_index = partition.entry_index,
                .disk_offset = range.value().offset,
                .length = range.value().length,
                .volume_guid_path =
                    volume_bindings[binding.value()].volume_device_path,
              });
        } else {
          layout.locked_partitions.push_back(
              OnlineDirectLockedPartition{
                  .partition_index = partition.entry_index,
                  .disk_offset = range.value().offset,
                  .length = range.value().length,
                  .volume_guid_path =
                      volume_bindings[binding.value()].volume_device_path,
                  .file_system = file_system.value() ==
                          clonecore::BasicDataFileSystem::fat32
                      ? OnlineDirectLockedFileSystem::fat32
                      : OnlineDirectLockedFileSystem::exfat,
              });
        }
      } else if (
          partition.type_guid == clonecore::gpt_type_efi_system()) {
        const auto fat = validate_boot_read(
            read_only_source, range.value(), false);
        if (!fat) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              fat.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (
          partition.type_guid ==
          clonecore::gpt_type_windows_recovery()) {
        const auto ntfs = validate_boot_read(
            read_only_source, range.value(), true);
        if (!ntfs) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              ntfs.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (
          partition.type_guid !=
          clonecore::gpt_type_microsoft_reserved()) {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインGPTパーティション種別",
                L"Snapshot整合性を保証できないGPTパーティションがあります"));
      }
    }
  } else if (
      observed_source.partition_style == diskmodel::PartitionStyle::mbr) {
    const auto mbr = clonecore::parse_mbr(read_only_source);
    if (!mbr) {
      return clonecore::Result<OnlineDirectSourceLayout>::failure(
          mbr.error());
    }
    layout.partition_style = OnlineDirectClonePartitionStyle::mbr;
    layout.static_physical_ranges.push_back(
        clonecore::ByteRange{.offset = 0, .length = 512});
    for (const auto& partition : mbr.value().partitions) {
      const auto range = make_partition_range(
          partition.first_lba,
          partition.sector_count,
          mbr.value().logical_sector_size,
          read_only_source.size_bytes(),
          L"オンラインMBRパーティション");
      if (!range) {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            range.error());
      }
      if (partition.type == 0x07) {
        const auto file_system = classify_basic_file_system_read(
            read_only_source, range.value());
        if (!file_system) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              file_system.error());
        }
        if (file_system.value() ==
            clonecore::BasicDataFileSystem::fat32) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              direct_error(
                  clonecore::ErrorCode::unsupported_layout,
                  ERROR_NOT_SUPPORTED,
                  L"オンラインMBR 0x07ファイルシステム",
                  L"MBR 0x07上のFAT32は型と実形式が一致しないため拒否します"));
        }
        const auto binding = find_binding(
            partition.table_index, volume_bindings, used);
        if (!binding) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              binding.error());
        }
        used[binding.value()] = 1;
        if (file_system.value() ==
            clonecore::BasicDataFileSystem::ntfs) {
          layout.snapshot_partitions.push_back(
              OnlineDirectSnapshotPartition{
                .partition_index = partition.table_index,
                .disk_offset = range.value().offset,
                .length = range.value().length,
                .volume_guid_path =
                    volume_bindings[binding.value()].volume_device_path,
              });
        } else {
          layout.locked_partitions.push_back(
              OnlineDirectLockedPartition{
                  .partition_index = partition.table_index,
                  .disk_offset = range.value().offset,
                  .length = range.value().length,
                  .volume_guid_path =
                      volume_bindings[binding.value()].volume_device_path,
                  .file_system = OnlineDirectLockedFileSystem::exfat,
              });
        }
      } else if (partition.type == 0x27 && !partition.active) {
        const auto ntfs = validate_boot_read(
            read_only_source, range.value(), true);
        if (!ntfs) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              ntfs.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (
          (partition.type == 0x0B || partition.type == 0x0C) &&
          partition.active) {
        const auto fat = validate_boot_read(
            read_only_source, range.value(), false);
        if (!fat) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              fat.error());
        }
        layout.static_physical_ranges.push_back(range.value());
      } else if (partition.type == 0x0B || partition.type == 0x0C) {
        const auto fat = validate_boot_read(
            read_only_source, range.value(), false);
        if (!fat) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              fat.error());
        }
        const auto binding = find_binding(
            partition.table_index, volume_bindings, used);
        if (!binding) {
          return clonecore::Result<OnlineDirectSourceLayout>::failure(
              binding.error());
        }
        used[binding.value()] = 1;
        layout.locked_partitions.push_back(
            OnlineDirectLockedPartition{
                .partition_index = partition.table_index,
                .disk_offset = range.value().offset,
                .length = range.value().length,
                .volume_guid_path =
                    volume_bindings[binding.value()].volume_device_path,
                .file_system = OnlineDirectLockedFileSystem::fat32,
            });
      } else {
        return clonecore::Result<OnlineDirectSourceLayout>::failure(
            direct_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"オンラインMBRパーティション種別",
                L"Snapshot整合性を保証できないMBRパーティションがあります"));
      }
    }
  } else {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        direct_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"オンライン直接クローン コピー元形式",
        L"GPTまたはMBRの基本ディスクだけを扱えます"));
  }

  const auto all_used = reject_unused_or_duplicate_bindings(
      volume_bindings, used);
  if (!all_used) {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        all_used.error());
  }
  const auto ranges = validate_layout_ranges(layout);
  if (!ranges) {
    return clonecore::Result<OnlineDirectSourceLayout>::failure(
        ranges.error());
  }
  return clonecore::Result<OnlineDirectSourceLayout>::success(
      std::move(layout));
}

clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
make_online_direct_composite_reader(
    const clonecore::ISourceDiskReader* read_only_physical_source,
    std::vector<OnlineDirectSnapshotReader> snapshot_readers,
    std::vector<clonecore::ByteRange> static_physical_ranges) {
  return make_online_direct_composite_reader(
      read_only_physical_source,
      std::move(snapshot_readers),
      {},
      std::move(static_physical_ranges));
}

clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
make_online_direct_composite_reader(
    const clonecore::ISourceDiskReader* read_only_physical_source,
    std::vector<OnlineDirectSnapshotReader> snapshot_readers,
    std::vector<OnlineDirectLockedReader> locked_readers,
    std::vector<clonecore::ByteRange> static_physical_ranges) {
  if (read_only_physical_source == nullptr ||
      (snapshot_readers.empty() && locked_readers.empty()) ||
      static_physical_ranges.empty()) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        direct_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"オンラインSnapshot合成Reader生成",
            L"物理Reader、整合性Reader、または固定領域がありません"));
  }
  std::sort(
      snapshot_readers.begin(),
      snapshot_readers.end(),
      [](const auto& left, const auto& right) {
        return left.disk_offset < right.disk_offset;
      });
  std::sort(
      locked_readers.begin(),
      locked_readers.end(),
      [](const auto& left, const auto& right) {
        return left.disk_offset < right.disk_offset;
      });
  std::sort(
      static_physical_ranges.begin(),
      static_physical_ranges.end(),
      range_less);
  std::uint64_t previous_end{};
  for (std::size_t index = 0; index < snapshot_readers.size(); ++index) {
    const auto& route = snapshot_readers[index];
    std::uint64_t end{};
    if (!route.reader || route.length == 0 ||
        !checked_add(route.disk_offset, route.length, end) ||
        end > read_only_physical_source->size_bytes() ||
        (index != 0 && route.disk_offset < previous_end) ||
        route.reader->size_bytes() != route.length ||
        route.reader->logical_sector_size() !=
            read_only_physical_source->logical_sector_size()) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          direct_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"オンラインSnapshot Reader再確認",
              L"Snapshot Readerの範囲、寸法、セクター、または重複が不正です"));
    }
    previous_end = end;
  }
  previous_end = 0;
  for (std::size_t index = 0; index < locked_readers.size(); ++index) {
    const auto& route = locked_readers[index];
    std::uint64_t end{};
    if (!route.reader || route.length == 0 ||
        !checked_add(route.disk_offset, route.length, end) ||
        end > read_only_physical_source->size_bytes() ||
        (index != 0 && route.disk_offset < previous_end) ||
        route.reader->size_bytes() != route.length ||
        route.reader->logical_sector_size() !=
            read_only_physical_source->logical_sector_size()) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          direct_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"オンラインVolume lock Reader再確認",
              L"Volume lock Readerの範囲、寸法、セクター、または重複が不正です"));
    }
    previous_end = end;
  }
  previous_end = 0;
  for (std::size_t index = 0;
       index < static_physical_ranges.size(); ++index) {
    const auto& fixed = static_physical_ranges[index];
    std::uint64_t end{};
    if (fixed.length == 0 || !checked_add(fixed.offset, fixed.length, end) ||
        end > read_only_physical_source->size_bytes() ||
        (index != 0 && fixed.offset < previous_end)) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          direct_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"オンライン固定領域Reader再確認",
              L"固定領域がディスク境界外または重複しています"));
    }
    previous_end = end;
  }
  for (const auto& snapshot : snapshot_readers) {
    const std::uint64_t snapshot_end =
        snapshot.disk_offset + snapshot.length;
    for (const auto& fixed : static_physical_ranges) {
      const std::uint64_t fixed_end = fixed.offset + fixed.length;
      if (snapshot.disk_offset < fixed_end && fixed.offset < snapshot_end) {
        return clonecore::Result<
            std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
            direct_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"オンライン合成Reader経路分離",
                L"Snapshot領域と固定領域が重複しています"));
      }
    }
    for (const auto& locked : locked_readers) {
      const std::uint64_t locked_end = locked.disk_offset + locked.length;
      if (snapshot.disk_offset < locked_end &&
          locked.disk_offset < snapshot_end) {
        return clonecore::Result<
            std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
            direct_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"オンライン合成Reader経路分離",
                L"Snapshot領域とVolume lock領域が重複しています"));
      }
    }
  }
  for (const auto& locked : locked_readers) {
    const std::uint64_t locked_end = locked.disk_offset + locked.length;
    for (const auto& fixed : static_physical_ranges) {
      const std::uint64_t fixed_end = fixed.offset + fixed.length;
      if (locked.disk_offset < fixed_end && fixed.offset < locked_end) {
        return clonecore::Result<
            std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
            direct_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"オンライン合成Reader経路分離",
                L"Volume lock領域と固定領域が重複しています"));
      }
    }
  }
  std::unique_ptr<clonecore::ISourceDiskReader> reader =
      std::make_unique<OnlineDirectCompositeReader>(
          read_only_physical_source,
          std::move(snapshot_readers),
          std::move(locked_readers),
          std::move(static_physical_ranges));
  return clonecore::Result<
      std::unique_ptr<clonecore::ISourceDiskReader>>::success(
      std::move(reader));
}

clonecore::Result<OnlineDirectCloneReport>
execute_online_direct_clone(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  const auto valid = validate_dependencies(request, dependencies);
  if (!valid) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        valid.error());
  }

  auto initial = dependencies.reidentify_clone(
      request.expected_source,
      request.expected_target,
      request.confirmation);
  if (!initial) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        initial.error());
  }
  const auto initial_safe = validate_observation(initial.value());
  if (!initial_safe) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        initial_safe.error());
  }
  const auto initial_layouts = validate_reviewed_layouts(
      request, initial.value(), L"開始時");
  if (!initial_layouts) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        initial_layouts.error());
  }

  auto source = dependencies.open_read_only_source(
      request.expected_source);
  if (!source) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        source.error());
  }
  if (!source.value().reader) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        direct_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_HANDLE,
            L"オンライン直接クローン コピー元Reader",
            L"検証済みの読取り専用物理Readerがありません"));
  }
  const auto source_identity = clonecore::validate_stable_identity(
      request.expected_source,
      source.value().observed.identity,
      L"オンライン直接クローン コピー元");
  if (!source_identity) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        source_identity.error());
  }

  std::vector<clonecore::VolumeBitmapBinding> volume_bindings;
  std::vector<std::uint32_t> connected_mbr_signatures;
  if (source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    const auto gpt = clonecore::parse_gpt(*source.value().reader);
    if (!gpt) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          gpt.error());
    }
    auto bindings = dependencies.query_gpt_bindings(
        source.value().observed.observed, gpt.value());
    if (!bindings) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          bindings.error());
    }
    volume_bindings = bindings.take_value();
  } else if (
      source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::mbr) {
    const auto mbr = clonecore::parse_mbr(*source.value().reader);
    if (!mbr) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          mbr.error());
    }
    auto bindings = dependencies.query_mbr_bindings(
        source.value().observed.observed, mbr.value());
    if (!bindings) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          bindings.error());
    }
    volume_bindings = bindings.take_value();
    auto signatures = dependencies.collect_mbr_signatures(
        request.expected_source, mbr.value());
    if (!signatures) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          signatures.error());
    }
    connected_mbr_signatures = signatures.take_value();
  } else {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        direct_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"オンライン直接クローン コピー元形式",
            L"GPTまたはMBRコピー元だけを扱えます"));
  }

  auto layout = build_online_direct_source_layout(
      source.value().observed.observed,
      *source.value().reader,
      volume_bindings);
  if (!layout) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        layout.error());
  }
  vssrequester::WorkflowRequest workflow{
      .administrator = request.administrator,
  };
  workflow.volumes.reserve(layout.value().snapshot_partitions.size());
  for (const auto& partition : layout.value().snapshot_partitions) {
    workflow.volumes.push_back(vssrequester::VolumeRequest{
        .volume_guid_path = partition.volume_guid_path,
        .file_system = L"NTFS",
    });
  }

  bool copy_callback_called = false;
  bool destructive_phase_started = false;
  bool target_offline_verified = false;
  const bool boot_finalization_required =
      request.expected_source.is_system_disk;
  bool boot_finalization_completed = false;
  std::optional<OnlineDirectCloneEngineReport> engine_report;
  const bool used_vss_snapshot = !layout.value().snapshot_partitions.empty();
  const auto copy_consistent_source =
      [&](const vssrequester::SnapshotCopyContext& snapshot_context) {
        try {
          const auto& mappings = snapshot_context.mappings;
        if (copy_callback_called ||
            snapshot_context.snapshot_set_id.empty() ||
            mappings.size() != layout.value().snapshot_partitions.size()) {
          return clonecore::Status::failure(direct_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"オンライン直接クローン Snapshot対応件数",
              L"Snapshotコピーは1回だけ、計画と同じ件数でなければなりません"));
        }
        copy_callback_called = true;

        std::vector<OnlineDirectSnapshotReader> snapshot_readers;
        std::vector<clonecore::SnapshotVolumeBitmapBinding>
            bitmap_bindings;
        snapshot_readers.reserve(mappings.size());
        bitmap_bindings.reserve(mappings.size());
        for (std::size_t index = 0;
             index < mappings.size(); ++index) {
          const auto& partition =
              layout.value().snapshot_partitions[index];
          if (mappings[index].snapshot_id.empty()) {
            return clonecore::Status::failure(direct_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"オンライン直接クローン Snapshot ID",
                L"Snapshot IDが空のためコピーを開始できません"));
          }
          auto reader = dependencies.open_snapshot_reader(
              vssrequester::SnapshotVolumeOpenRequest{
                  .snapshot_device_path =
                      mappings[index].snapshot_device_path,
                  .expected_size_bytes = partition.length,
                  .logical_sector_size =
                      request.expected_source.logical_sector_size,
              });
          if (!reader) {
            return clonecore::Status::failure(reader.error());
          }
          snapshot_readers.push_back(OnlineDirectSnapshotReader{
              .partition_index = partition.partition_index,
              .disk_offset = partition.disk_offset,
              .length = partition.length,
              .reader = reader.take_value(),
          });
          bitmap_bindings.push_back(
              clonecore::SnapshotVolumeBitmapBinding{
                  .partition_entry_index = partition.partition_index,
                  .snapshot_device_path =
                      mappings[index].snapshot_device_path,
              });
        }
        std::vector<OnlineDirectLockedReader> locked_readers;
        locked_readers.reserve(layout.value().locked_partitions.size());
        for (const auto& partition : layout.value().locked_partitions) {
          auto reader = dependencies.open_locked_volume(
              OnlineDirectLockedVolumeOpenRequest{
                  .physical_disk_number =
                      source.value().observed.observed.disk_number,
                  .partition_index = partition.partition_index,
                  .disk_offset = partition.disk_offset,
                  .length = partition.length,
                  .logical_sector_size =
                      request.expected_source.logical_sector_size,
                  .volume_guid_path = partition.volume_guid_path,
                  .expected_file_system = partition.file_system,
              });
          if (!reader || !reader.value()) {
            return clonecore::Status::failure(
                reader
                    ? direct_error(
                          clonecore::ErrorCode::internal_error,
                          ERROR_INVALID_HANDLE,
                          L"オンラインFAT/exFAT Volume lock Reader",
                          L"Volume lock openerが空のReaderを返しました")
                    : reader.error());
          }
          locked_readers.push_back(OnlineDirectLockedReader{
              .partition_index = partition.partition_index,
              .disk_offset = partition.disk_offset,
              .length = partition.length,
              .file_system = partition.file_system,
              .reader = reader.take_value(),
          });
        }
        auto composite = make_online_direct_composite_reader(
            source.value().reader.get(),
            std::move(snapshot_readers),
            std::move(locked_readers),
            layout.value().static_physical_ranges);
        if (!composite) {
          return clonecore::Status::failure(composite.error());
        }
        std::unique_ptr<clonecore::INtfsUsedRangeProvider> bitmap_provider;
        if (bitmap_bindings.empty()) {
          bitmap_provider =
              std::make_unique<RejectingNtfsUsedRangeProvider>();
        } else {
          auto made_provider =
              dependencies.make_snapshot_bitmap_provider(
                  std::move(bitmap_bindings));
          if (!made_provider || !made_provider.value()) {
            return clonecore::Status::failure(
                made_provider
                    ? direct_error(
                          clonecore::ErrorCode::internal_error,
                          ERROR_INVALID_HANDLE,
                          L"オンラインSnapshot Bitmap Provider",
                          L"Bitmap Provider Factoryが空を返しました")
                    : made_provider.error());
          }
          bitmap_provider = made_provider.take_value();
        }

        // This is the final source/target reidentification immediately before
        // the first target state change. Rebuild the source route plan from
        // the still-open physical reader to reject a layout change that the
        // stable device identity alone cannot detect.
        auto final_observation = dependencies.reidentify_clone(
            request.expected_source,
            request.expected_target,
            request.confirmation);
        if (!final_observation) {
          return clonecore::Status::failure(final_observation.error());
        }
        const auto final_safe =
            validate_observation(final_observation.value());
        if (!final_safe) {
          return final_safe;
        }
        const auto final_reviewed_layouts = validate_reviewed_layouts(
            request, final_observation.value(), L"書込み直前");
        if (!final_reviewed_layouts) {
          return final_reviewed_layouts;
        }
        auto current_layout = build_online_direct_source_layout(
            final_observation.value().source,
            *composite.value(),
            volume_bindings);
        if (!current_layout ||
            !same_source_layout(layout.value(), current_layout.value())) {
          return clonecore::Status::failure(
              current_layout
                  ? direct_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_INVALID_DATA,
                        L"オンライン直接クローン レイアウト再検査",
                        L"Snapshot取得後にコピー元レイアウトが変更されました")
                   : current_layout.error());
        }

        std::unique_ptr<vssrequester::VssDiffAreaOperationMonitor>
            diff_area_monitor;
        OnlineDirectCloneRequest active_request = request;
        if (used_vss_snapshot && dependencies.make_diff_area_monitor) {
          if (clonecore::disk_operation_cancellation_requested(
                  request.callbacks)) {
            return clonecore::Status::failure(direct_error(
                clonecore::ErrorCode::cancelled,
                ERROR_CANCELLED,
                L"オンライン直接クローン VSS差分領域初回poll",
                L"初回target変更前に取消要求を確認しました"));
          }
          auto made_monitor =
              dependencies.make_diff_area_monitor(snapshot_context);
          if (!made_monitor || !made_monitor.value()) {
            return clonecore::Status::failure(
                made_monitor
                    ? direct_error(
                          clonecore::ErrorCode::internal_error,
                          ERROR_INVALID_HANDLE,
                          L"オンライン直接クローン VSS差分領域monitor",
                          L"製品monitor factoryが空のmonitorを返しました")
                    : made_monitor.error());
          }
          diff_area_monitor = made_monitor.take_value();
          const auto monitored = diff_area_monitor->initial_poll();
          if (!monitored) {
            return monitored;
          }
          active_request.callbacks =
              diff_area_monitor->callbacks(request.callbacks);
        }

        const auto offline = dependencies.set_clone_target_offline(
            request.expected_source,
            request.expected_target,
            request.confirmation,
            true);
        if (!offline) {
          return offline;
        }
        destructive_phase_started = true;
        auto target = dependencies.open_offline_target(
            request.expected_target, request.confirmation);
        if (!target) {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              target.error(), protected_offline));
        }
        const auto identities = clonecore::validate_clone_identities(
            request.expected_source,
            final_observation.value().source_identity,
            request.expected_target,
            target.value().observed.target_identity,
            request.confirmation);
        if (!identities) {
          target.value().target.reset();
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              identities.error(), protected_offline));
        }
        auto opened_target_layout = imageformat::
            hash_tsumugi_physical_restore_target_layout_v1(
                target.value().observed.target);
        if (!opened_target_layout ||
            opened_target_layout.value() !=
                request.expected_target_layout_hash) {
          target.value().target.reset();
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              opened_target_layout
                  ? direct_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_DEVICE_REINITIALIZATION_NEEDED,
                        L"オンライン直接クローン 開いたコピー先レイアウト照合",
                        L"同じ対象ハンドルのパーティション形式または配置が最終確認と一致しません")
                  : opened_target_layout.error(),
              protected_offline));
        }

        auto executed = dependencies.execute_clone_engine(
            OnlineDirectCloneEngineContext{
                .partition_style = layout.value().partition_style,
                .request = &active_request,
                .observed_source =
                    &final_observation.value().source_identity,
                .observed_target =
                    &target.value().observed.target_identity,
                .source = composite.value().get(),
                .target = target.value().target.get(),
                .snapshot_bitmap_provider =
                    bitmap_provider.get(),
                .connected_mbr_signatures =
                    connected_mbr_signatures,
            });
        target.value().target.reset();
        if (!executed) {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              executed.error(), protected_offline));
        }
        const std::uint64_t accounted_partitions =
            static_cast<std::uint64_t>(
                executed.value().copied_partition_count) +
            static_cast<std::uint64_t>(
                executed.value().recreated_partition_count);
        if (executed.value().copied_data_bytes == 0U ||
            executed.value().copied_data_bytes >
                request.expected_source.size_bytes ||
            accounted_partitions !=
                final_observation.value().source.partitions.size() ||
            !executed.value().read_back_verified ||
            all_zero(executed.value().verified_write_digest) ||
            !executed.value().partition_table_committed) {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          return clonecore::Status::failure(append_offline_failure(
              direct_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"オンライン直接クローン 完了検証",
                  L"転送量、全パーティション、書込み証跡、読戻し検証、またはパーティション表確定が完了していません"),
              protected_offline));
        }

        if (boot_finalization_required) {
          const auto brought_online =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  false);
          if (!brought_online) {
            const auto protected_offline =
                dependencies.set_physical_target_offline(
                    request.expected_target,
                    request.confirmation,
                    true);
            return clonecore::Status::failure(append_offline_failure(
                brought_online.error(), protected_offline));
          }
          const auto boot_finalized = dependencies.finalize_target_boot(
              request.expected_target, layout.value().partition_style);
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          if (!boot_finalized) {
            return clonecore::Status::failure(append_offline_failure(
                boot_finalized.error(), protected_offline));
          }
          if (!protected_offline) {
            return protected_offline;
          }
          boot_finalization_completed = true;
        } else {
          const auto protected_offline =
              dependencies.set_physical_target_offline(
                  request.expected_target,
                  request.confirmation,
                  true);
          if (!protected_offline) {
            return protected_offline;
          }
        }
        target_offline_verified = true;
        if (diff_area_monitor) {
          auto monitored = diff_area_monitor->completion_poll();
          if (monitored) {
            monitored = vssrequester::
                validate_completed_vss_diff_area_operation_evidence(
                    diff_area_monitor->evidence());
          }
          if (!monitored) {
            return monitored;
          }
        }
        engine_report = executed.take_value();
        return clonecore::success_status();
        } catch (...) {
          auto error = direct_error(
              clonecore::ErrorCode::internal_error,
              ERROR_UNHANDLED_EXCEPTION,
              L"オンライン直接クローン 実行例外",
              L"依存処理が例外を送出したため安全側に停止しました");
          if (!destructive_phase_started) {
            return clonecore::Status::failure(std::move(error));
          }
          const auto protected_offline =
              reprotect_target_offline_after_exception(
                  request, dependencies);
          return clonecore::Status::failure(append_offline_failure(
              std::move(error), protected_offline));
        }
      };

  const auto workflow_report = [&]()
      -> clonecore::Result<vssrequester::WorkflowReport> {
    if (used_vss_snapshot) {
      return dependencies.run_snapshot_workflow(
          workflow,
          request.async_wait,
          request.logger,
          copy_consistent_source);
    }
    const vssrequester::SnapshotCopyContext locked_context{
        .snapshot_set_id = L"LOCKED-VOLUME-CONSISTENCY",
    };
    const auto copied = copy_consistent_source(locked_context);
    if (!copied) {
      return clonecore::Result<vssrequester::WorkflowReport>::failure(
          copied.error());
    }
    return clonecore::Result<vssrequester::WorkflowReport>::success(
        vssrequester::WorkflowReport{
            .snapshot_set_id = locked_context.snapshot_set_id,
            .volume_count = layout.value().locked_partitions.size(),
            .snapshot_data_copied = true,
            .backup_completed = false,
            .snapshots_deleted = false,
        });
  }();

  if (!workflow_report) {
    if (!destructive_phase_started) {
      return clonecore::Result<OnlineDirectCloneReport>::failure(
          workflow_report.error());
    }
    const auto protected_offline =
        reprotect_target_offline_after_exception(
            request, dependencies);
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        append_offline_failure(
            workflow_report.error(), protected_offline));
  }
  if (!copy_callback_called || !engine_report.has_value() ||
      !target_offline_verified ||
      (boot_finalization_required && !boot_finalization_completed) ||
      !workflow_report.value().snapshot_data_copied ||
      (used_vss_snapshot && !workflow_report.value().backup_completed) ||
      (used_vss_snapshot && !workflow_report.value().snapshots_deleted)) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        direct_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_STATE,
            L"オンライン直接クローン 整合性完了状態",
            L"Clone Engine、VSS/Volume lock、Snapshot削除、またはoffline検証が完了していません"));
  }
  return clonecore::Result<OnlineDirectCloneReport>::success(
      OnlineDirectCloneReport{
          .partition_style = layout.value().partition_style,
          .copied_data_bytes = engine_report->copied_data_bytes,
          .copied_partition_count =
              engine_report->copied_partition_count,
          .recreated_partition_count =
              engine_report->recreated_partition_count,
          .verified_write_digest =
              engine_report->verified_write_digest,
          .read_back_verified = engine_report->read_back_verified,
          .partition_table_committed =
              engine_report->partition_table_committed,
          .snapshot_backup_completed =
              workflow_report.value().backup_completed,
          .snapshots_deleted =
              workflow_report.value().snapshots_deleted,
          .used_vss_snapshot = used_vss_snapshot,
          .locked_volume_count = static_cast<std::uint32_t>(
              layout.value().locked_partitions.size()),
          .source_consistency_verified = true,
          .target_left_offline = true,
          .boot_finalization_required = boot_finalization_required,
          .boot_finalization_completed = boot_finalization_completed,
      });
}

OnlineDirectCloneDependencies
make_online_direct_clone_windows_dependencies() {
  return OnlineDirectCloneDependencies{
          .reidentify_clone_selection =
              [](const clonecore::StableDiskIdentity& source,
                 const clonecore::StableDiskIdentity& target) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                return diskmodel::reidentify_physical_clone_selection(
                    source, target, *inventory);
              },
          .reidentify_clone =
              [](const clonecore::StableDiskIdentity& source,
                 const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                return diskmodel::reidentify_physical_clone(
                    source,
                    target,
                    confirmation,
                    *inventory);
              },
          .open_read_only_source =
              [](const clonecore::StableDiskIdentity& source) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        source);
              },
          .query_gpt_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::GptDisk& gpt) {
                return diskmodel::query_windows_volume_bitmap_bindings(
                    disk, gpt);
              },
          .query_mbr_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::MbrDisk& mbr) {
                return diskmodel::query_windows_volume_bitmap_bindings(
                    disk, mbr);
              },
          .run_snapshot_workflow =
              [](const vssrequester::WorkflowRequest& workflow,
                 const vssrequester::AsyncWaitOptions& async_wait,
                 const clonecore::Logger* logger,
                 vssrequester::SnapshotCopyCallback callback) {
                vssrequester::WindowsVssBackend backend(
                    vssrequester::WindowsVssBackendOptions{
                        .async_wait = async_wait,
                        .copy_snapshot_data = std::move(callback),
                        .logger = logger,
                    });
                return vssrequester::execute_backup_workflow(
                    workflow, backend);
              },
          .open_snapshot_reader =
              [](const vssrequester::SnapshotVolumeOpenRequest& open) {
                return vssrequester::
                    open_snapshot_volume_reader_with_windows_apis(open);
              },
          .open_locked_volume =
              open_locked_file_system_volume_with_windows_apis,
          .make_snapshot_bitmap_provider =
              [](std::vector<clonecore::SnapshotVolumeBitmapBinding>
                     bindings) {
                std::unique_ptr<clonecore::INtfsUsedRangeProvider> provider =
                    std::make_unique<
                        clonecore::WindowsSnapshotVolumeBitmapProvider>(
                        std::move(bindings));
                return clonecore::Result<std::unique_ptr<
                    clonecore::INtfsUsedRangeProvider>>::success(
                    std::move(provider));
              },
          .set_clone_target_offline =
              [](const clonecore::StableDiskIdentity& source,
                 const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation,
                 const bool offline) {
                return diskmodel::
                    set_verified_target_offline_with_windows_apis(
                        source, target, confirmation, offline);
              },
          .set_physical_target_offline =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation,
                 const bool offline) {
                return diskmodel::
                    set_verified_physical_target_offline_with_windows_apis(
                        target, confirmation, offline);
              },
          .open_offline_target =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation) {
                return diskmodel::
                    open_verified_physical_target_with_windows_apis(
                        target, confirmation);
              },
          .collect_mbr_signatures =
              collect_connected_mbr_signatures_with_windows_apis,
          .execute_clone_engine = execute_clone_engine_with_native_core,
          .finalize_target_boot =
              [](const clonecore::StableDiskIdentity& target,
                 const OnlineDirectClonePartitionStyle style) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                auto finalizer = bootrepair::
                    make_windows_clone_boot_finalization_service(*inventory);
                const auto finalized = finalizer->execute(
                    bootrepair::CloneBootFinalizationRequest{
                        .expected_target = target,
                        .expected_style =
                            style == OnlineDirectClonePartitionStyle::gpt
                            ? diskmodel::PartitionStyle::gpt
                            : diskmodel::PartitionStyle::mbr,
                    });
                if (!finalized) {
                  return clonecore::Status::failure(finalized.error());
                }
                return clonecore::success_status();
              },
      };
}

clonecore::Result<OnlineDirectCloneReport>
execute_online_direct_clone_with_windows_apis(
    const OnlineDirectCloneRequest& request) {
  auto source_token =
      vssrequester::encode_vss_diff_area_source_epoch_token(
          std::span<const std::byte>(request.expected_source_layout_hash));
  if (!source_token || !request.diff_area_review_callback) {
    return clonecore::Result<OnlineDirectCloneReport>::failure(
        source_token
            ? direct_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"オンライン直接クローン VSS差分領域監視",
                  L"source epochまたは利用者review callbackがありません")
            : source_token.error());
  }
  const std::wstring expected_source_token = source_token.take_value();
  auto dependencies = make_online_direct_clone_windows_dependencies();
  dependencies.make_diff_area_monitor =
      [request, expected_source_token](
          const vssrequester::SnapshotCopyContext& context) {
        return vssrequester::make_windows_vss_diff_area_operation_monitor(
            context,
            vssrequester::WindowsVssDiffAreaOperationMonitorOptions{
                .expected_source_identity_token = expected_source_token,
                .probe_source_identity =
                    [request, expected_source_token](
                        const vssrequester::VssDiffAreaSnapshotBinding&) {
                      auto inventory =
                          diskmodel::make_windows_disk_inventory_provider();
                      auto observed =
                          diskmodel::reidentify_physical_clone(
                              request.expected_source,
                              request.expected_target,
                              request.confirmation,
                              *inventory);
                      if (!observed) {
                        return clonecore::Result<std::wstring>::failure(
                            observed.error());
                      }
                      auto status = clonecore::validate_clone_identities(
                          request.expected_source,
                          observed.value().source_identity,
                          request.expected_target,
                          observed.value().target_identity,
                          request.confirmation);
                      if (status) {
                        status = validate_reviewed_layouts(
                            request,
                            observed.value(),
                            L"VSS差分領域source epoch再識別");
                      }
                      return status
                          ? clonecore::Result<std::wstring>::success(
                                expected_source_token)
                          : clonecore::Result<std::wstring>::failure(
                                status.error());
                    },
                .review_callback = request.diff_area_review_callback,
                .logger = request.logger,
            });
      };
  return execute_online_direct_clone(request, dependencies);
}

}  // namespace ytec::windowsapp

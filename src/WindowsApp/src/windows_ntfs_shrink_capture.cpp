#include "ytec/windowsapp/windows_ntfs_shrink_capture.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kHashBlockBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kDismTimeoutMilliseconds =
    6U * 60U * 60U * 1000U;
constexpr std::array<std::byte, 8U> kWimSignature{
    static_cast<std::byte>('M'), static_cast<std::byte>('S'),
    static_cast<std::byte>('W'), static_cast<std::byte>('I'),
    static_cast<std::byte>('M'), std::byte{0}, std::byte{0}, std::byte{0},
};

clonecore::Error capture_error(
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
  return clonecore::Result<T>::failure(capture_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(capture_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool align_up(
    const std::uint64_t value,
    const std::uint64_t alignment,
    std::uint64_t& result) noexcept {
  if (alignment == 0U ||
      value > (std::numeric_limits<std::uint64_t>::max)() -
          (alignment - 1U)) {
    return false;
  }
  result = ((value + alignment - 1U) / alignment) * alignment;
  return true;
}

bool is_power_of_two(const std::uint32_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

bool equal_path(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
      (value >= L'a' && value <= L'f') ||
      (value >= L'A' && value <= L'F');
}

bool is_volume_guid_path(const std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (path.size() != 49U || !path.starts_with(prefix) ||
      path[47U] != L'}' || path[48U] != L'\\') {
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
  auto suffix = path.substr(prefix.size());
  if (suffix.ends_with(L'\\')) {
    suffix.remove_suffix(1U);
  }
  return !suffix.empty() &&
      std::all_of(suffix.begin(), suffix.end(), [](const wchar_t value) {
        return value >= L'0' && value <= L'9';
      });
}

bool all_zero(const imageformat::Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

template <typename Enum>
bool has_flag(const Enum value, const Enum flag) noexcept {
  using Integer = std::underlying_type_t<Enum>;
  return (static_cast<Integer>(value) & static_cast<Integer>(flag)) != 0;
}

bool selected(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return has_flag(
      partition.flags,
      imageformat::TsumugiManifestPartitionFlags::selected);
}

bool static_exact_raw_file_system(
    const imageformat::TsumugiManifestFileSystem file_system) noexcept {
  return file_system == imageformat::TsumugiManifestFileSystem::unknown ||
      file_system == imageformat::TsumugiManifestFileSystem::none;
}

bool same_partition(
    const imageformat::TsumugiManifestPartition& left,
    const imageformat::TsumugiManifestPartition& right) noexcept {
  return left.source_table_index == right.source_table_index &&
      left.source_partition_number == right.source_partition_number &&
      left.role == right.role && left.file_system == right.file_system &&
      left.flags == right.flags &&
      left.source_offset == right.source_offset &&
      left.source_size == right.source_size &&
      left.used_bytes == right.used_bytes &&
      left.minimum_target_bytes == right.minimum_target_bytes &&
      left.planned_target_bytes == right.planned_target_bytes &&
      left.payload_logical_offset == right.payload_logical_offset &&
      left.payload_logical_length == right.payload_logical_length &&
      left.payload_encoding == right.payload_encoding &&
      left.payload_format_version == right.payload_format_version &&
      left.cluster_size == right.cluster_size &&
      left.type_id == right.type_id && left.unique_id == right.unique_id &&
      left.name_utf8 == right.name_utf8 && left.label_utf8 == right.label_utf8;
}

enum class CaptureBindingKind : std::uint8_t {
  snapshot_wim,
  static_exact_raw,
};

struct CaptureBinding final {
  imageformat::TsumugiManifestPartition partition;
  CaptureBindingKind kind{CaptureBindingKind::snapshot_wim};
  std::wstring original_volume_guid_path;
};

clonecore::Result<std::vector<CaptureBinding>> validate_plan(
    const WindowsNtfsShrinkCapturePlan& plan,
    const WindowsNtfsShrinkCaptureDependencies& dependencies) {
  const auto source = clonecore::validate_stable_identity(
      plan.source_disk, plan.source_disk, L"Windows縮小WIMコピー元");
  if (!source) {
    return clonecore::Result<std::vector<CaptureBinding>>::failure(
        source.error());
  }
  const auto& manifest = plan.reviewed_manifest;
  const auto model_hash = imageformat::hash_tsumugi_source_model_v1(
      plan.source_disk.model);
  const auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
      plan.source_disk.serial_suffix,
      plan.source_disk.device_instance_id);
  if (!model_hash || !serial_hash) {
    return clonecore::Result<std::vector<CaptureBinding>>::failure(
        model_hash ? serial_hash.error() : model_hash.error());
  }
  if (!dependencies.open_read_only_source ||
      !dependencies.resolve_snapshot_volumes ||
      !dependencies.create_wim_staging || !dependencies.capture_wim ||
      manifest.mode != imageformat::TsumugiManifestMode::shrink ||
      manifest.source_disk_size != plan.source_disk.size_bytes ||
      manifest.logical_sector_size !=
          plan.source_disk.logical_sector_size ||
      manifest.source_disk_size == 0U ||
      !is_power_of_two(manifest.logical_sector_size) ||
      manifest.source_disk_size % manifest.logical_sector_size != 0U ||
      manifest.source_model_hash != model_hash.value() ||
      manifest.source_serial_hash != serial_hash.value() ||
      !all_zero(manifest.source_state_hash) ||
      (manifest.partition_style !=
           imageformat::TsumugiManifestPartitionStyle::mbr &&
       manifest.partition_style !=
           imageformat::TsumugiManifestPartitionStyle::gpt) ||
      !is_power_of_two(manifest.physical_sector_size) ||
      manifest.physical_sector_size < manifest.logical_sector_size ||
      manifest.partition_snapshot.empty() || manifest.partitions.empty() ||
      manifest.partitions.size() >
          imageformat::kTsumugiManifestMaximumPartitions) {
    return failure<std::vector<CaptureBinding>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows縮小WIM計画",
        L"縮小マニフェスト、コピー元寸法、または必須Platform依存が不正です");
  }
  constexpr std::uint32_t known_manifest_flags =
      static_cast<std::uint32_t>(
          imageformat::TsumugiManifestFlags::source_contains_windows) |
      static_cast<std::uint32_t>(
          imageformat::TsumugiManifestFlags::
              bitlocker_source_was_unlocked) |
      static_cast<std::uint32_t>(
          imageformat::TsumugiManifestFlags::partition_selection) |
      static_cast<std::uint32_t>(
          imageformat::TsumugiManifestFlags::
              automatic_surplus_allocation);
  if ((static_cast<std::uint32_t>(manifest.flags) &
       ~known_manifest_flags) != 0U) {
    return failure<std::vector<CaptureBinding>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows縮小WIM manifest flags",
        L"未知の必須manifest flagを拒否しました");
  }
  if (has_flag(
          manifest.flags,
          imageformat::TsumugiManifestFlags::bitlocker_source_was_unlocked)) {
    return failure<std::vector<CaptureBinding>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows縮小WIM BitLocker境界",
        L"このNTFS限定sliceではBitLocker元ディスクをcaptureできません");
  }

  const auto inspected = imageformat::inspect_partition_snapshot_v1(
      manifest.partition_snapshot);
  const auto expected_style =
      manifest.partition_style ==
              imageformat::TsumugiManifestPartitionStyle::gpt
          ? imageformat::PartitionTableStyle::gpt
          : imageformat::PartitionTableStyle::mbr;
  if (!inspected || inspected.value().style != expected_style ||
      inspected.value().source_disk_size != manifest.source_disk_size ||
      inspected.value().logical_sector_size !=
          manifest.logical_sector_size) {
    return failure<std::vector<CaptureBinding>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小WIMパーティション表",
        L"認証対象のPartitionSnapshotがマニフェスト寸法と一致しません");
  }

  std::map<std::uint32_t, std::wstring> selections;
  for (const auto& volume : plan.snapshot_volumes) {
    if (volume.source_table_index == 0U ||
        !is_volume_guid_path(volume.original_volume_guid_path) ||
        !selections
             .emplace(
                 volume.source_table_index,
                 volume.original_volume_guid_path)
             .second) {
      return failure<std::vector<CaptureBinding>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_DUP_NAME,
          L"Windows縮小WIM Volume選択",
          L"Volume GUID対応が不正または重複しています");
    }
  }

  std::vector<CaptureBinding> bindings;
  bindings.reserve(manifest.partitions.size());
  std::size_t wim_count{};
  for (std::size_t index = 0U; index < manifest.partitions.size(); ++index) {
    const auto& partition = manifest.partitions[index];
    constexpr std::uint32_t known_partition_flags =
        static_cast<std::uint32_t>(
            imageformat::TsumugiManifestPartitionFlags::selected) |
        static_cast<std::uint32_t>(
            imageformat::TsumugiManifestPartitionFlags::required) |
        static_cast<std::uint32_t>(
            imageformat::TsumugiManifestPartitionFlags::active) |
        static_cast<std::uint32_t>(
            imageformat::TsumugiManifestPartitionFlags::contains_windows) |
        static_cast<std::uint32_t>(
            imageformat::TsumugiManifestPartitionFlags::
                bitlocker_was_unlocked);
    if ((static_cast<std::uint32_t>(partition.flags) &
         ~known_partition_flags) != 0U) {
      return failure<std::vector<CaptureBinding>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小WIM partition flags",
          L"未知のpartition flagを拒否しました");
    }
    if (has_flag(
            partition.flags,
            imageformat::TsumugiManifestPartitionFlags::
                bitlocker_was_unlocked)) {
      return failure<std::vector<CaptureBinding>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小WIM BitLockerパーティション",
          L"BitLocker解除済み印を持つパーティションはこのsliceで扱えません");
    }
    if (!selected(partition)) {
      continue;
    }
    std::uint64_t end{};
    if (partition.source_table_index == 0U ||
        partition.source_partition_number == 0U ||
        partition.source_size == 0U ||
        partition.source_offset % manifest.logical_sector_size != 0U ||
        partition.source_size % manifest.logical_sector_size != 0U ||
        !checked_add(partition.source_offset, partition.source_size, end) ||
        end > manifest.source_disk_size ||
        partition.payload_logical_offset != 0U ||
        partition.payload_logical_length != 0U ||
        partition.payload_encoding !=
            imageformat::TsumugiManifestPayloadEncoding::exact_raw ||
        partition.payload_format_version != 0U ||
        partition.minimum_target_bytes == 0U ||
        partition.planned_target_bytes < partition.minimum_target_bytes) {
      return failure<std::vector<CaptureBinding>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Windows縮小WIM partition extent",
          L"選択済みパーティションの識別、範囲、整列、または未確定payloadが不正です");
    }
    for (const auto& previous : bindings) {
      std::uint64_t previous_end{};
      static_cast<void>(checked_add(
          previous.partition.source_offset,
          previous.partition.source_size,
          previous_end));
      if (previous.partition.source_table_index ==
              partition.source_table_index ||
          previous.partition.source_partition_number ==
              partition.source_partition_number ||
          (partition.source_offset < previous_end &&
           previous.partition.source_offset < end)) {
        return failure<std::vector<CaptureBinding>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"Windows縮小WIM partition一意性",
            L"選択済みパーティションの番号または範囲が重複しています");
      }
    }

    CaptureBinding binding{.partition = partition};
    if (partition.file_system ==
        imageformat::TsumugiManifestFileSystem::ntfs) {
      const auto volume = selections.find(partition.source_table_index);
      if (volume == selections.end()) {
        return failure<std::vector<CaptureBinding>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Windows縮小WIM Volume extent結合",
            L"選択NTFSとVolume GUIDの対応がありません");
      }
      binding.kind = CaptureBindingKind::snapshot_wim;
      binding.original_volume_guid_path = volume->second;
      ++wim_count;
    } else if (static_exact_raw_file_system(partition.file_system)) {
      if (selections.contains(partition.source_table_index)) {
        return failure<std::vector<CaptureBinding>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Windows縮小RAW Volume境界",
            L"静的exact RAW領域をVSS Volumeとして対応付けできません");
      }
      binding.kind = CaptureBindingKind::static_exact_raw;
    } else {
      return failure<std::vector<CaptureBinding>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小WIM filesystem境界",
          L"このsliceはNTFS WIMと未知／filesystemなしの静的exact RAWだけを扱います");
    }
    bindings.push_back(std::move(binding));
  }
  if (bindings.empty() || wim_count == 0U ||
      wim_count != selections.size()) {
    return failure<std::vector<CaptureBinding>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows縮小WIM capture集合",
        L"1件以上のNTFSと過不足のないVolume GUID対応が必要です");
  }
  return clonecore::Result<std::vector<CaptureBinding>>::success(
      std::move(bindings));
}

clonecore::Status validate_runtime_partitions(
    const std::span<const imageformat::TsumugiManifestPartition> current,
    const imageformat::TsumugiManifest& reviewed) {
  if (current.size() != reviewed.partitions.size()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小WIM reviewed partition結合",
        L"VSS開始後にパーティション件数が計画から変化しました");
  }
  for (std::size_t index = 0U; index < current.size(); ++index) {
    if (!same_partition(current[index], reviewed.partitions[index])) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小WIM reviewed extent結合",
          L"VSS開始後に選択、filesystem、またはpartition extentが計画から変化しました");
    }
  }
  return clonecore::success_status();
}

struct BoundSnapshot final {
  std::uint32_t source_table_index{};
  std::wstring original_volume_guid_path;
  std::wstring snapshot_id;
  std::wstring snapshot_device_path;
};

clonecore::Result<std::vector<BoundSnapshot>> bind_snapshot_context(
    const vssrequester::SnapshotCopyContext& context,
    const std::span<const CaptureBinding> bindings) {
  const std::size_t expected_count = static_cast<std::size_t>(std::count_if(
      bindings.begin(), bindings.end(), [](const CaptureBinding& binding) {
        return binding.kind == CaptureBindingKind::snapshot_wim;
      }));
  if (context.snapshot_set_id.empty() ||
      context.mappings.size() != expected_count) {
    return failure<std::vector<BoundSnapshot>>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小WIM Snapshot set結合",
        L"同一Snapshot setのIDまたはVolume件数が計画と一致しません");
  }
  std::vector<BoundSnapshot> result;
  result.reserve(expected_count);
  for (const auto& binding : bindings) {
    if (binding.kind != CaptureBindingKind::snapshot_wim) {
      continue;
    }
    const vssrequester::SnapshotMapping* match = nullptr;
    for (const auto& mapping : context.mappings) {
      if (!equal_path(
              binding.original_volume_guid_path,
              mapping.original_volume_guid_path)) {
        continue;
      }
      if (match != nullptr) {
        return failure<std::vector<BoundSnapshot>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"Windows縮小WIM Snapshot重複",
            L"一つのVolumeに複数のSnapshot対応があります");
      }
      match = &mapping;
    }
    if (match == nullptr || match->snapshot_id.empty() ||
        !is_snapshot_device_path(match->snapshot_device_path)) {
      return failure<std::vector<BoundSnapshot>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Windows縮小WIM Snapshot ID結合",
          L"Volume GUID、Snapshot ID、またはSnapshotデバイスが不足しています");
    }
    for (const auto& previous : result) {
      if (equal_path(previous.snapshot_id, match->snapshot_id) ||
          equal_path(
              previous.snapshot_device_path,
              match->snapshot_device_path)) {
        return failure<std::vector<BoundSnapshot>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"Windows縮小WIM Snapshot一意性",
            L"複数Volumeが同じSnapshot IDまたはデバイスを共有しています");
      }
    }
    result.push_back(BoundSnapshot{
        .source_table_index = binding.partition.source_table_index,
        .original_volume_guid_path = binding.original_volume_guid_path,
        .snapshot_id = match->snapshot_id,
        .snapshot_device_path = match->snapshot_device_path,
    });
  }
  return clonecore::Result<std::vector<BoundSnapshot>>::success(
      std::move(result));
}

const CaptureBinding* find_binding(
    const std::span<const CaptureBinding> bindings,
    const std::uint32_t table_index) noexcept {
  const auto found = std::find_if(
      bindings.begin(), bindings.end(), [&](const CaptureBinding& value) {
        return value.partition.source_table_index == table_index;
      });
  return found == bindings.end() ? nullptr : &*found;
}

const BoundSnapshot* find_snapshot(
    const std::span<const BoundSnapshot> snapshots,
    const std::uint32_t table_index) noexcept {
  const auto found = std::find_if(
      snapshots.begin(), snapshots.end(), [&](const BoundSnapshot& value) {
        return value.source_table_index == table_index;
      });
  return found == snapshots.end() ? nullptr : &*found;
}

imageformat::PartitionTableStyle snapshot_style(
    const imageformat::TsumugiManifestPartitionStyle style) noexcept {
  return style == imageformat::TsumugiManifestPartitionStyle::gpt
      ? imageformat::PartitionTableStyle::gpt
      : imageformat::PartitionTableStyle::mbr;
}

clonecore::Status validate_observed_source(
    const WindowsNtfsShrinkCapturePlan& plan,
    const std::span<const CaptureBinding> bindings,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const WindowsNtfsShrinkVolumeResolver& resolver) {
  const auto identity = clonecore::validate_stable_identity(
      plan.source_disk,
      source.observed.identity,
      L"Windows縮小WIMコピー元再識別");
  if (!identity) {
    return identity;
  }
  const auto expected_disk_style =
      plan.reviewed_manifest.partition_style ==
              imageformat::TsumugiManifestPartitionStyle::gpt
          ? diskmodel::PartitionStyle::gpt
          : diskmodel::PartitionStyle::mbr;
  if (!source.reader ||
      source.reader->size_bytes() != plan.source_disk.size_bytes ||
      source.reader->logical_sector_size() !=
          plan.source_disk.logical_sector_size ||
      source.observed.observed.size_bytes != plan.source_disk.size_bytes ||
      source.observed.observed.logical_sector_size !=
          plan.source_disk.logical_sector_size ||
      source.observed.observed.physical_sector_size !=
          plan.reviewed_manifest.physical_sector_size ||
      source.observed.observed.partition_style != expected_disk_style) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows縮小WIMコピー元geometry",
        L"再オープンしたコピー元の寸法またはpartition styleが計画と一致しません");
  }
  for (const auto& binding : bindings) {
    const auto matches = std::count_if(
        source.observed.observed.partitions.begin(),
        source.observed.observed.partitions.end(),
        [&](const diskmodel::PartitionInfo& partition) {
          return partition.number ==
                     binding.partition.source_partition_number &&
              partition.offset_bytes == binding.partition.source_offset &&
              partition.size_bytes == binding.partition.source_size;
        });
    if (matches != 1) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows縮小WIM partition extent再確認",
          L"コピー元のpartition number、offset、lengthを一意に再確認できません");
    }
    if (binding.kind == CaptureBindingKind::snapshot_wim) {
      auto boot_sector = source.reader->read(
          binding.partition.source_offset,
          plan.source_disk.logical_sector_size);
      if (!boot_sector ||
          boot_sector.value().size() !=
              plan.source_disk.logical_sector_size) {
        return boot_sector
            ? status_failure(
                  clonecore::ErrorCode::io_failed,
                  ERROR_HANDLE_EOF,
                  L"Windows縮小NTFS boot sector",
                  L"選択NTFSのboot sectorを完全に読み取れません")
            : clonecore::Status::failure(boot_sector.error());
      }
      auto geometry = clonecore::parse_ntfs_geometry(
          boot_sector.value(),
          plan.source_disk.logical_sector_size,
          binding.partition.source_size);
      if (!geometry) {
        return status_failure(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Windows縮小NTFS／BitLocker実体確認",
            L"選択extentの実体を通常NTFSと確認できません。BitLockerまたは誤ったfilesystem指定を拒否しました");
      }
    }
  }
  auto current_snapshot = imageformat::capture_partition_snapshot_v1(
      *source.reader,
      snapshot_style(plan.reviewed_manifest.partition_style));
  if (!current_snapshot) {
    return clonecore::Status::failure(current_snapshot.error());
  }
  if (current_snapshot.value() !=
      plan.reviewed_manifest.partition_snapshot) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_CRC,
        L"Windows縮小WIM PartitionSnapshot再確認",
        L"読取り専用コピー元のpartition tableがレビュー時点から変化しました");
  }

  std::vector<diskmodel::VolumePartitionLocation> locations;
  for (const auto& binding : bindings) {
    if (binding.kind == CaptureBindingKind::snapshot_wim) {
      locations.push_back({
          .table_index = binding.partition.source_table_index,
          .offset_bytes = binding.partition.source_offset,
      });
    }
  }
  auto volumes = resolver(source.observed.observed, locations);
  if (!volumes) {
    return clonecore::Status::failure(volumes.error());
  }
  if (volumes.value().size() != locations.size()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小WIM Volume extent再確認",
        L"選択NTFSと単一ディスクVolumeの件数が一致しません");
  }
  for (const auto& binding : bindings) {
    if (binding.kind != CaptureBindingKind::snapshot_wim) {
      continue;
    }
    const auto matches = std::count_if(
        volumes.value().begin(),
        volumes.value().end(),
        [&](const clonecore::VolumeBitmapBinding& volume) {
          return volume.partition_entry_index ==
                     binding.partition.source_table_index &&
              equal_path(
                  volume.volume_device_path,
                  binding.original_volume_guid_path);
        });
    if (matches != 1) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DRIVE,
          L"Windows縮小WIM Volume GUID再確認",
          L"現在のpartition extentとレビュー済みVolume GUIDを一意に結合できません");
    }
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> hash_reader_region(
    const clonecore::ISourceDiskReader& source,
    const std::uint64_t offset,
    const std::uint64_t length) {
  std::uint64_t end{};
  if (length == 0U || !checked_add(offset, length, end) ||
      end > source.size_bytes()) {
    return failure<imageformat::Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小RAW fingerprint範囲",
        L"exact RAW範囲がコピー元境界外です");
  }
  return imageformat::sha256_from_reader(
      length,
      kHashBlockBytes,
      [&source, offset](
          const std::uint64_t relative,
          const std::size_t amount) {
        std::uint64_t absolute{};
        if (!checked_add(offset, relative, absolute)) {
          return failure<std::vector<std::byte>>(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Windows縮小RAW fingerprint位置",
              L"exact RAW読取り位置が64bit上限を超えます");
        }
        return source.read(absolute, amount);
      });
}

clonecore::Result<imageformat::Sha256Digest> hash_staged_wim(
    const IWindowsNtfsShrinkWimStaging& staging,
    const std::uint32_t table_index,
    const std::uint64_t length) {
  return imageformat::sha256_from_reader(
      length,
      kHashBlockBytes,
      [&staging, table_index](
          const std::uint64_t offset,
          const std::size_t amount) {
        return staging.read_wim(table_index, offset, amount);
      });
}

struct PayloadProof final {
  CaptureBinding binding;
  std::optional<BoundSnapshot> snapshot;
  std::uint64_t length{};
  imageformat::Sha256Digest digest{};
};

void append_u16(
    std::vector<std::byte>& output,
    const std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value & 0xFFU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(
    std::vector<std::byte>& output,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(
    std::vector<std::byte>& output,
    const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

clonecore::Status append_wstring(
    std::vector<std::byte>& output,
    const std::wstring_view value) {
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Windows縮小状態Hash文字列",
        L"Snapshot識別文字列が上限を超えます");
  }
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    append_u16(output, static_cast<std::uint16_t>(character));
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> derive_state_hash(
    const WindowsNtfsShrinkCapturePlan& plan,
    const std::wstring_view snapshot_set_id,
    const std::span<const PayloadProof> proofs) {
  constexpr std::string_view domain =
      "YTEC-WINDOWS-NTFS-SHRINK-CAPTURE-STATE-V1";
  std::vector<std::byte> material;
  material.reserve(1024U + proofs.size() * 256U);
  for (const char value : domain) {
    material.push_back(static_cast<std::byte>(value));
  }
  append_u64(material, plan.source_disk.size_bytes);
  append_u32(material, plan.source_disk.logical_sector_size);
  const auto model_hash = imageformat::hash_tsumugi_source_model_v1(
      plan.source_disk.model);
  const auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
      plan.source_disk.serial_suffix,
      plan.source_disk.device_instance_id);
  if (!model_hash || !serial_hash) {
    return clonecore::Result<imageformat::Sha256Digest>::failure(
        model_hash ? serial_hash.error() : model_hash.error());
  }
  material.insert(
      material.end(), model_hash.value().begin(), model_hash.value().end());
  material.insert(
      material.end(), serial_hash.value().begin(), serial_hash.value().end());
  auto status = append_wstring(material, snapshot_set_id);
  if (!status) {
    return clonecore::Result<imageformat::Sha256Digest>::failure(
        status.error());
  }
  const auto snapshot_hash = imageformat::sha256(
      plan.reviewed_manifest.partition_snapshot);
  if (!snapshot_hash) {
    return clonecore::Result<imageformat::Sha256Digest>::failure(
        snapshot_hash.error());
  }
  material.insert(
      material.end(), snapshot_hash.value().begin(), snapshot_hash.value().end());

  std::vector<const PayloadProof*> sorted;
  sorted.reserve(proofs.size());
  for (const auto& proof : proofs) {
    sorted.push_back(&proof);
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
    return left->binding.partition.source_table_index <
        right->binding.partition.source_table_index;
  });
  append_u32(material, static_cast<std::uint32_t>(sorted.size()));
  for (const auto* proof : sorted) {
    append_u32(material, proof->binding.partition.source_table_index);
    append_u32(material, proof->binding.partition.source_partition_number);
    append_u64(material, proof->binding.partition.source_offset);
    append_u64(material, proof->binding.partition.source_size);
    append_u64(material, proof->length);
    append_u16(
        material,
        proof->binding.kind == CaptureBindingKind::snapshot_wim ? 1U : 2U);
    material.insert(
        material.end(), proof->digest.begin(), proof->digest.end());
    if (proof->snapshot.has_value()) {
      status = append_wstring(
          material, proof->snapshot->original_volume_guid_path);
      if (status) {
        status = append_wstring(material, proof->snapshot->snapshot_id);
      }
      if (status) {
        status = append_wstring(
            material, proof->snapshot->snapshot_device_path);
      }
      if (!status) {
        return clonecore::Result<imageformat::Sha256Digest>::failure(
            status.error());
      }
    } else {
      append_u32(material, 0U);
      append_u32(material, 0U);
      append_u32(material, 0U);
    }
  }
  return imageformat::sha256(material);
}

struct SessionSegment final {
  WindowsShrinkCapturedPayload descriptor;
  imageformat::Sha256Digest initial_digest{};
};

class WindowsNtfsShrinkCapturedSession final
    : public IWindowsShrinkCapturedSession {
 public:
  WindowsNtfsShrinkCapturedSession(
      WindowsNtfsShrinkCapturePlan plan,
      std::vector<CaptureBinding> bindings,
      diskmodel::ReadOnlyPhysicalDiskHandle source,
      WindowsNtfsShrinkSourceOpener source_opener,
      WindowsNtfsShrinkVolumeResolver volume_resolver,
      std::unique_ptr<IWindowsNtfsShrinkWimStaging> staging,
      std::vector<SessionSegment> segments,
      imageformat::Sha256Digest model_hash,
      imageformat::Sha256Digest serial_hash,
      imageformat::Sha256Digest state_hash)
      : plan_(std::move(plan)),
        bindings_(std::move(bindings)),
        source_(std::move(source)),
        source_opener_(std::move(source_opener)),
        volume_resolver_(std::move(volume_resolver)),
        staging_(std::move(staging)),
        segments_(std::move(segments)),
        model_hash_(model_hash),
        serial_hash_(serial_hash),
        state_hash_(state_hash) {
    descriptors_.reserve(segments_.size());
    for (const auto& segment : segments_) {
      descriptors_.push_back(segment.descriptor);
    }
  }

  ~WindowsNtfsShrinkCapturedSession() override {
    if (staging_ != nullptr && !staging_discarded_) {
      static_cast<void>(staging_->discard_owned());
    }
  }

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return plan_.source_disk.size_bytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return plan_.source_disk.logical_sector_size;
  }

  [[nodiscard]] imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    return model_hash_;
  }

  [[nodiscard]] imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    return serial_hash_;
  }

  [[nodiscard]] imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    return state_hash_;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::scoped_lock lock(mutex_);
    if (staging_discarded_) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_HANDLE,
          L"Windows縮小capture session読取り",
          L"所有WIM staging破棄後のsessionは読み取れません");
    }
    if (length == 0U) {
      return clonecore::Result<std::vector<std::byte>>::success({});
    }
    std::uint64_t request_end{};
    if (!checked_add(offset, static_cast<std::uint64_t>(length), request_end) ||
        request_end > size_bytes()) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_HANDLE_EOF,
          L"Windows縮小capture session範囲",
          L"session読取りが仮想source namespace境界外です");
    }
    for (const auto& segment : segments_) {
      std::uint64_t segment_end{};
      if (!checked_add(
              segment.descriptor.session_source_offset,
              segment.descriptor.length,
              segment_end) ||
          offset < segment.descriptor.session_source_offset ||
          request_end > segment_end) {
        continue;
      }
      const std::uint64_t relative =
          offset - segment.descriptor.session_source_offset;
      if (segment.descriptor.kind ==
          WindowsShrinkCapturedPayloadKind::vss_snapshot_wim) {
        return staging_->read_wim(
            segment.descriptor.source_table_index, relative, length);
      }
      std::uint64_t source_offset{};
      if (!checked_add(
              segment.descriptor.original_source_offset,
              relative,
              source_offset)) {
        return failure<std::vector<std::byte>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Windows縮小exact RAW session位置",
            L"元ディスク読取り位置が64bit上限を超えます");
      }
      return source_.reader->read(source_offset, length);
    }
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小capture session対応",
        L"要求範囲が一つのWIMまたはexact RAW payloadへ完全に収まりません");
  }

  [[nodiscard]] std::span<const WindowsShrinkCapturedPayload>
  payloads() const noexcept override {
    return descriptors_;
  }

  [[nodiscard]] const clonecore::StableDiskIdentity&
  observed_source_disk() const noexcept override {
    return source_.observed.identity;
  }

  [[nodiscard]] clonecore::Status discard_owned_staging() noexcept override {
    std::scoped_lock lock(mutex_);
    if (staging_discarded_) {
      return discard_failure_.has_value()
          ? clonecore::Status::failure(*discard_failure_)
          : clonecore::success_status();
    }
    std::optional<clonecore::Error> primary;
    try {
      auto current = source_opener_(plan_.source_disk);
      if (!current) {
        primary = current.error();
      } else {
        const auto current_layout = validate_observed_source(
            plan_, bindings_, current.value(), volume_resolver_);
        if (!current_layout) {
          primary = current_layout.error();
        } else {
          for (const auto& segment : segments_) {
            if (segment.descriptor.kind !=
                WindowsShrinkCapturedPayloadKind::
                    locked_read_only_exact_raw) {
              continue;
            }
            auto digest = hash_reader_region(
                *current.value().reader,
                segment.descriptor.original_source_offset,
                segment.descriptor.length);
            if (!digest) {
              primary = digest.error();
              break;
            }
            if (digest.value() != segment.initial_digest) {
              primary = capture_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_CRC,
                  L"Windows縮小exact RAW最終fingerprint",
                  L"静的exact RAW領域がWIM capture中に変化しました");
              break;
            }
          }
        }
      }
    } catch (...) {
      primary = capture_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Windows縮小コピー元最終再確認",
          L"コピー元再確認Adapterが例外を返しました");
    }

    const auto cleanup = staging_->discard_owned();
    if (!cleanup) {
      if (primary.has_value()) {
        primary->message +=
            L"。所有WIM staging破棄にも失敗しました: " +
            cleanup.error().message;
      } else {
        primary = cleanup.error();
      }
    } else {
      staging_discarded_ = true;
    }
    if (primary.has_value()) {
      discard_failure_ = *primary;
      return clonecore::Status::failure(std::move(*primary));
    }
    return clonecore::success_status();
  }

 private:
  WindowsNtfsShrinkCapturePlan plan_;
  std::vector<CaptureBinding> bindings_;
  diskmodel::ReadOnlyPhysicalDiskHandle source_;
  WindowsNtfsShrinkSourceOpener source_opener_;
  WindowsNtfsShrinkVolumeResolver volume_resolver_;
  std::unique_ptr<IWindowsNtfsShrinkWimStaging> staging_;
  std::vector<SessionSegment> segments_;
  std::vector<WindowsShrinkCapturedPayload> descriptors_;
  imageformat::Sha256Digest model_hash_{};
  imageformat::Sha256Digest serial_hash_{};
  imageformat::Sha256Digest state_hash_{};
  mutable std::mutex mutex_;
  bool staging_discarded_{};
  std::optional<clonecore::Error> discard_failure_;
};

bool descendant_of_scratch(
    const std::wstring_view scratch,
    const std::wstring_view candidate) noexcept {
  if (scratch.empty() || candidate.size() <= scratch.size() ||
      candidate.find(L"..") != std::wstring_view::npos ||
      !equal_path(candidate.substr(0U, scratch.size()), scratch)) {
    return false;
  }
  const wchar_t boundary = candidate[scratch.size()];
  return scratch.ends_with(L'\\') || scratch.ends_with(L'/') ||
      boundary == L'\\' || boundary == L'/';
}

std::wstring snapshot_capture_root(std::wstring path) {
  if (!path.ends_with(L'\\')) {
    path.push_back(L'\\');
  }
  return path;
}

clonecore::Result<std::unique_ptr<IWindowsShrinkCapturedSession>>
cleanup_capture_failure(
    clonecore::Error error,
    IWindowsNtfsShrinkWimStaging& staging) {
  const auto cleanup = staging.discard_owned();
  if (!cleanup) {
    error.message += L"。所有WIM staging破棄にも失敗しました: " +
        cleanup.error().message;
  }
  return clonecore::Result<
      std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
      std::move(error));
}

clonecore::Result<std::unique_ptr<IWindowsShrinkCapturedSession>>
capture_snapshot_payloads(
    const WindowsNtfsShrinkCapturePlan& plan,
    const std::vector<CaptureBinding>& bindings,
    const WindowsNtfsShrinkCaptureDependencies& dependencies,
    const vssrequester::SnapshotCopyContext& context,
    const std::span<const imageformat::TsumugiManifestPartition> partitions,
    const WindowsShrinkWorkPaths& work_paths,
    const clonecore::DiskOperationCallbacks& callbacks) {
  const auto runtime = validate_runtime_partitions(
      partitions, plan.reviewed_manifest);
  if (!runtime) {
    return clonecore::Result<
        std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
        runtime.error());
  }
  auto snapshots = bind_snapshot_context(context, bindings);
  if (!snapshots) {
    return clonecore::Result<
        std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
        snapshots.error());
  }
  if (work_paths.scratch_directory.empty()) {
    return failure<std::unique_ptr<IWindowsShrinkCapturedSession>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows縮小WIM canonical scratch",
        L"既存work placement guardで確定したscratchがありません");
  }
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return failure<std::unique_ptr<IWindowsShrinkCapturedSession>>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"Windows縮小WIM capture開始",
        L"最初のI/O前に安全に取り消されました");
  }

  auto source = dependencies.open_read_only_source(plan.source_disk);
  if (!source) {
    return clonecore::Result<
        std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
        source.error());
  }
  const auto observed = validate_observed_source(
      plan, bindings, source.value(), dependencies.resolve_snapshot_volumes);
  if (!observed) {
    return clonecore::Result<
        std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
        observed.error());
  }

  std::map<std::uint32_t, imageformat::Sha256Digest> raw_hashes;
  for (const auto& binding : bindings) {
    if (binding.kind != CaptureBindingKind::static_exact_raw) {
      continue;
    }
    auto digest = hash_reader_region(
        *source.value().reader,
        binding.partition.source_offset,
        binding.partition.source_size);
    if (!digest) {
      return clonecore::Result<
          std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
          digest.error());
    }
    raw_hashes.emplace(
        binding.partition.source_table_index, digest.take_value());
  }

  auto staging = dependencies.create_wim_staging(
      work_paths.scratch_directory);
  if (!staging || !staging.value()) {
    return clonecore::Result<
        std::unique_ptr<IWindowsShrinkCapturedSession>>::failure(
        staging ? capture_error(
                      clonecore::ErrorCode::internal_error,
                      ERROR_INVALID_HANDLE,
                      L"Windows縮小WIM staging factory",
                      L"staging factoryが空の所有objectを返しました")
                : staging.error());
  }

  std::vector<PayloadProof> proofs;
  proofs.reserve(bindings.size());
  for (const auto& binding : bindings) {
    if (binding.kind == CaptureBindingKind::static_exact_raw) {
      proofs.push_back(PayloadProof{
          .binding = binding,
          .length = binding.partition.source_size,
          .digest = raw_hashes.at(binding.partition.source_table_index),
      });
      continue;
    }
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::cancelled,
              ERROR_CANCELLED,
              L"Windows縮小WIM Volume境界取消",
              L"次のSnapshot Volume capture前に安全に取り消されました"),
          *staging.value());
    }
    const auto* snapshot = find_snapshot(
        snapshots.value(), binding.partition.source_table_index);
    if (snapshot == nullptr) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"Windows縮小WIM Snapshot lookup",
              L"検証済みSnapshot対応が失われました"),
          *staging.value());
    }
    auto image_path = staging.value()->reserve_wim_path(
        binding.partition.source_table_index);
    if (!image_path) {
      return cleanup_capture_failure(image_path.error(), *staging.value());
    }
    if (!descendant_of_scratch(
            work_paths.scratch_directory, image_path.value()) ||
        image_path.value().size() <= 4U ||
        _wcsicmp(
            image_path.value().c_str() + image_path.value().size() - 4U,
            L".wim") != 0) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_NAME,
              L"Windows縮小WIM staging path",
              L"WIM stagingはcanonical scratch配下の.wimでなければなりません"),
          *staging.value());
    }
    const windowsdism::DismCaptureRequest request{
        .source_root = snapshot_capture_root(
            snapshot->snapshot_device_path),
        .image_path = image_path.value(),
        .scratch_directory = work_paths.scratch_directory,
        .image_name = L"Y-TEC Tsumugi Drive volume " +
            std::to_wstring(binding.partition.source_table_index),
    };
    auto dism = dependencies.capture_wim(request);
    if (!dism) {
      return cleanup_capture_failure(dism.error(), *staging.value());
    }
    auto length = staging.value()->seal_wim(
        binding.partition.source_table_index);
    if (!length) {
      return cleanup_capture_failure(length.error(), *staging.value());
    }
    if (dism.value().exit_code != 0U ||
        !dism.value().microsoft_signature_verified) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"Windows縮小WIM DISM信頼境界",
              L"System32 DISMの成功と前後Authenticode検証を確認できません"),
          *staging.value());
    }
    if (length.value() < kWimSignature.size()) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_BAD_FORMAT,
              L"Windows縮小WIM長",
              L"DISMが作成したWIMが空またはheader未満です"),
          *staging.value());
    }
    auto signature = staging.value()->read_wim(
        binding.partition.source_table_index, 0U, kWimSignature.size());
    if (!signature || signature.value().size() != kWimSignature.size() ||
        !std::equal(
            signature.value().begin(),
            signature.value().end(),
            kWimSignature.begin())) {
      return cleanup_capture_failure(
          signature
              ? capture_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_BAD_FORMAT,
                    L"Windows縮小WIM header",
                    L"DISM出力がMicrosoft WIM signatureではありません")
              : signature.error(),
          *staging.value());
    }
    auto digest = hash_staged_wim(
        *staging.value(),
        binding.partition.source_table_index,
        length.value());
    if (!digest) {
      return cleanup_capture_failure(digest.error(), *staging.value());
    }
    proofs.push_back(PayloadProof{
        .binding = binding,
        .snapshot = *snapshot,
        .length = length.value(),
        .digest = digest.take_value(),
    });
  }

  auto state_hash = derive_state_hash(
      plan, context.snapshot_set_id, proofs);
  if (!state_hash || all_zero(state_hash.value())) {
    return cleanup_capture_failure(
        state_hash
            ? capture_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows縮小capture state hash",
                  L"Snapshot ID、コピー元identity、extent、payloadを結合できません")
            : state_hash.error(),
        *staging.value());
  }
  auto model_hash = imageformat::hash_tsumugi_source_model_v1(
      source.value().observed.identity.model);
  auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
      source.value().observed.identity.serial_suffix,
      source.value().observed.identity.device_instance_id);
  if (!model_hash || !serial_hash) {
    return cleanup_capture_failure(
        model_hash ? serial_hash.error() : model_hash.error(),
        *staging.value());
  }

  std::map<std::uint32_t, const PayloadProof*> proof_by_table;
  for (const auto& proof : proofs) {
    proof_by_table.emplace(
        proof.binding.partition.source_table_index, &proof);
  }
  std::vector<SessionSegment> segments;
  segments.reserve(proofs.size());
  std::uint64_t cursor{};
  for (const auto& partition : plan.reviewed_manifest.partitions) {
    if (!selected(partition)) {
      continue;
    }
    const auto proof = proof_by_table.find(partition.source_table_index);
    if (proof == proof_by_table.end()) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::internal_error,
              ERROR_NOT_FOUND,
              L"Windows縮小payload proof対応",
              L"選択済みpartitionのpayload proofがありません"),
          *staging.value());
    }
    if (proof->second->binding.kind ==
        CaptureBindingKind::static_exact_raw) {
      std::uint64_t aligned{};
      if (!align_up(
              cursor,
              plan.source_disk.logical_sector_size,
              aligned)) {
        return cleanup_capture_failure(
            capture_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_ARITHMETIC_OVERFLOW,
                L"Windows縮小RAW virtual offset",
                L"exact RAW session offsetを整列できません"),
            *staging.value());
      }
      cursor = aligned;
    }
    std::uint64_t end{};
    if (!checked_add(cursor, proof->second->length, end) ||
        end > plan.source_disk.size_bytes) {
      return cleanup_capture_failure(
          capture_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_DISK_FULL,
              L"Windows縮小payload virtual namespace",
              L"WIMとexact RAWの仮想payload合計がコピー元ディスク寸法を超えます"),
          *staging.value());
    }
    WindowsShrinkCapturedPayload descriptor{
        .source_table_index = partition.source_table_index,
        .kind = proof->second->binding.kind ==
                CaptureBindingKind::snapshot_wim
            ? WindowsShrinkCapturedPayloadKind::vss_snapshot_wim
            : WindowsShrinkCapturedPayloadKind::
                  locked_read_only_exact_raw,
        .original_source_offset =
            proof->second->binding.kind ==
                    CaptureBindingKind::static_exact_raw
                ? partition.source_offset
                : 0U,
        .session_source_offset = cursor,
        .length = proof->second->length,
    };
    if (proof->second->snapshot.has_value()) {
      descriptor.original_volume_guid_path =
          proof->second->snapshot->original_volume_guid_path;
      descriptor.snapshot_id = proof->second->snapshot->snapshot_id;
      descriptor.snapshot_device_path =
          proof->second->snapshot->snapshot_device_path;
    }
    segments.push_back(SessionSegment{
        .descriptor = std::move(descriptor),
        .initial_digest = proof->second->digest,
    });
    cursor = end;
  }

  std::unique_ptr<IWindowsShrinkCapturedSession> session =
      std::make_unique<WindowsNtfsShrinkCapturedSession>(
          plan,
          bindings,
          source.take_value(),
          dependencies.open_read_only_source,
          dependencies.resolve_snapshot_volumes,
          staging.take_value(),
          std::move(segments),
          model_hash.take_value(),
          serial_hash.take_value(),
          state_hash.take_value());
  return clonecore::Result<
      std::unique_ptr<IWindowsShrinkCapturedSession>>::success(
      std::move(session));
}

struct OwnedWim final {
  std::uint32_t table_index{};
  std::wstring path;
  clonecore::UniqueHandle handle;
  std::uint64_t length{};
  bool sealed{};
  bool deletion_requested{};
};

class WindowsNtfsShrinkWimStaging final
    : public IWindowsNtfsShrinkWimStaging {
 public:
  static clonecore::Result<std::unique_ptr<IWindowsNtfsShrinkWimStaging>>
  create(const std::wstring& scratch) {
    const DWORD scratch_attributes = GetFileAttributesW(scratch.c_str());
    if (scratch_attributes == INVALID_FILE_ATTRIBUTES ||
        (scratch_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (scratch_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return failure<std::unique_ptr<IWindowsNtfsShrinkWimStaging>>(
          clonecore::ErrorCode::unsupported_layout,
          scratch_attributes == INVALID_FILE_ATTRIBUTES
              ? GetLastError()
              : ERROR_REPARSE_TAG_INVALID,
          L"Windows縮小WIM scratch検証",
          L"canonical scratchは存在する通常フォルダーである必要があります");
    }
    static std::atomic_uint64_t sequence{};
    std::wstring directory;
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
      directory = scratch;
      if (!directory.ends_with(L'\\') && !directory.ends_with(L'/')) {
        directory.push_back(L'\\');
      }
      directory += L"YTEC-Tsumugi-WIM-";
      directory += std::to_wstring(GetCurrentProcessId());
      directory.push_back(L'-');
      directory += std::to_wstring(GetTickCount64());
      directory.push_back(L'-');
      directory += std::to_wstring(sequence.fetch_add(1U));
      if (CreateDirectoryW(directory.c_str(), nullptr) != FALSE) {
        break;
      }
      const DWORD native_code = GetLastError();
      if (native_code != ERROR_ALREADY_EXISTS) {
        return clonecore::Result<
            std::unique_ptr<IWindowsNtfsShrinkWimStaging>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"Windows縮小WIM所有directory作成",
                native_code));
      }
      directory.clear();
    }
    if (directory.empty()) {
      return failure<std::unique_ptr<IWindowsNtfsShrinkWimStaging>>(
          clonecore::ErrorCode::io_failed,
          ERROR_ALREADY_EXISTS,
          L"Windows縮小WIM所有directory作成",
          L"衝突しない所有directory名を確保できません");
    }
    clonecore::UniqueHandle directory_handle(CreateFileW(
        directory.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!directory_handle) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<
          std::unique_ptr<IWindowsNtfsShrinkWimStaging>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"Windows縮小WIM所有directory固定",
              native_code));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            directory_handle.get(),
            FileAttributeTagInfo,
            &tag,
            sizeof(tag)) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      const DWORD native_code = GetLastError();
      directory_handle.reset();
      return failure<std::unique_ptr<IWindowsNtfsShrinkWimStaging>>(
          clonecore::ErrorCode::unsupported_layout,
          native_code == ERROR_SUCCESS ? ERROR_REPARSE_TAG_INVALID
                                       : native_code,
          L"Windows縮小WIM所有directory属性",
          L"作成直後の所有directoryが通常directoryではありません");
    }
    std::unique_ptr<IWindowsNtfsShrinkWimStaging> result =
        std::unique_ptr<IWindowsNtfsShrinkWimStaging>(
            new WindowsNtfsShrinkWimStaging(
                std::move(directory), std::move(directory_handle)));
    return clonecore::Result<
        std::unique_ptr<IWindowsNtfsShrinkWimStaging>>::success(
        std::move(result));
  }

  ~WindowsNtfsShrinkWimStaging() override {
    if (!discarded_) {
      static_cast<void>(discard_owned());
    }
  }

  [[nodiscard]] clonecore::Result<std::wstring> reserve_wim_path(
      const std::uint32_t source_table_index) override {
    std::scoped_lock lock(mutex_);
    if (discarded_ || source_table_index == 0U ||
        std::any_of(
            files_.begin(), files_.end(), [&](const OwnedWim& file) {
              return file.table_index == source_table_index;
            })) {
      return failure<std::wstring>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_DUP_NAME,
          L"Windows縮小WIM path予約",
          L"破棄済みstagingまたは重複partition番号は予約できません");
    }
    std::wstring path = directory_ + L"\\volume-" +
        std::to_wstring(source_table_index) + L".wim";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const DWORD native_code = attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError()
        : ERROR_SUCCESS;
    if (attributes != INVALID_FILE_ATTRIBUTES ||
        (native_code != ERROR_FILE_NOT_FOUND &&
         native_code != ERROR_PATH_NOT_FOUND)) {
      return failure<std::wstring>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_EXISTS,
          L"Windows縮小WIM path所有確認",
          L"予約前からWIM pathが存在するため未知fileとして拒否しました");
    }
    files_.push_back(OwnedWim{
        .table_index = source_table_index,
        .path = path,
    });
    return clonecore::Result<std::wstring>::success(std::move(path));
  }

  [[nodiscard]] clonecore::Result<std::uint64_t> seal_wim(
      const std::uint32_t source_table_index) override {
    std::scoped_lock lock(mutex_);
    auto found = std::find_if(
        files_.begin(), files_.end(), [&](const OwnedWim& file) {
          return file.table_index == source_table_index;
        });
    if (discarded_ || found == files_.end() || found->sealed) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Windows縮小WIM seal",
          L"予約済み未sealのWIMだけを固定できます");
    }
    clonecore::UniqueHandle handle(CreateFileW(
        found->path.c_str(),
        GENERIC_READ | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Result<std::uint64_t>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"Windows縮小WIM読取り固定",
              GetLastError()));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !GetFileInformationByHandleEx(
            handle.get(), FileStandardInfo, &standard, sizeof(standard)) ||
        (tag.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        standard.DeletePending || standard.NumberOfLinks != 1U ||
        standard.EndOfFile.QuadPart <= 0) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_BAD_FORMAT,
          L"Windows縮小WIM regular file固定",
          L"DISM出力が一意な通常fileではありません");
    }
    found->length = static_cast<std::uint64_t>(
        standard.EndOfFile.QuadPart);
    found->handle = std::move(handle);
    found->sealed = true;
    return clonecore::Result<std::uint64_t>::success(found->length);
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_wim(
      const std::uint32_t source_table_index,
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::scoped_lock lock(mutex_);
    const auto found = std::find_if(
        files_.begin(), files_.end(), [&](const OwnedWim& file) {
          return file.table_index == source_table_index;
        });
    std::uint64_t end{};
    if (discarded_ || found == files_.end() || !found->sealed ||
        !found->handle ||
        length > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) ||
        !checked_add(offset, static_cast<std::uint64_t>(length), end) ||
        end > found->length) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_HANDLE_EOF,
          L"Windows縮小WIM読取り範囲",
          L"固定済みWIMの境界外または破棄後を読み取れません");
    }
    if (length == 0U) {
      return clonecore::Result<std::vector<std::byte>>::success({});
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(
            found->handle.get(), position, nullptr, FILE_BEGIN)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"Windows縮小WIM読取り位置",
              GetLastError()));
    }
    std::vector<std::byte> bytes(length);
    DWORD read{};
    if (!ReadFile(
            found->handle.get(),
            bytes.data(),
            static_cast<DWORD>(length),
            &read,
            nullptr) ||
        read != length) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"Windows縮小WIM完全読取り",
              native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code));
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  [[nodiscard]] clonecore::Status discard_owned() noexcept override {
    std::scoped_lock lock(mutex_);
    if (discarded_) {
      return clonecore::success_status();
    }
    if (terminal_cleanup_error_.has_value()) {
      return clonecore::Status::failure(*terminal_cleanup_error_);
    }
    std::optional<clonecore::Error> first_error;
    for (auto& file : files_) {
      if (file.sealed && file.handle) {
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        if (!SetFileInformationByHandle(
                file.handle.get(),
                FileDispositionInfo,
                &disposition,
                sizeof(disposition)) &&
            !first_error.has_value()) {
          first_error = clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"Windows縮小WIM所有file破棄",
              GetLastError());
        } else {
          file.deletion_requested = true;
        }
        file.handle.reset();
      } else if (!file.deletion_requested) {
        const DWORD attributes = GetFileAttributesW(file.path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            !first_error.has_value()) {
          first_error = capture_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_EXISTS,
              L"Windows縮小WIM未seal file保護",
              L"所有を証明できないWIM候補を削除せず残しました");
        } else if (attributes == INVALID_FILE_ATTRIBUTES) {
          const DWORD native_code = GetLastError();
          if (native_code != ERROR_FILE_NOT_FOUND &&
              native_code != ERROR_PATH_NOT_FOUND &&
              !first_error.has_value()) {
            first_error = clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"Windows縮小WIM未seal file確認",
                native_code);
          }
        }
      }
    }
    if (!first_error.has_value() && directory_handle_) {
      FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
      if (!SetFileInformationByHandle(
              directory_handle_.get(),
              FileDispositionInfo,
              &disposition,
              sizeof(disposition))) {
        first_error = clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Windows縮小WIM所有directory破棄",
            GetLastError());
      }
    }
    directory_handle_.reset();
    discarded_ = !first_error.has_value();
    if (first_error.has_value()) {
      terminal_cleanup_error_ = *first_error;
      return clonecore::Status::failure(std::move(*first_error));
    }
    return clonecore::success_status();
  }

 private:
  WindowsNtfsShrinkWimStaging(
      std::wstring directory,
      clonecore::UniqueHandle directory_handle)
      : directory_(std::move(directory)),
        directory_handle_(std::move(directory_handle)) {}

  std::wstring directory_;
  clonecore::UniqueHandle directory_handle_;
  std::vector<OwnedWim> files_;
  mutable std::mutex mutex_;
  bool discarded_{};
  std::optional<clonecore::Error> terminal_cleanup_error_;
};

clonecore::Result<std::wstring> query_system_directory() {
  std::vector<wchar_t> buffer(32768U, L'\0');
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Windows縮小WIM System32取得",
            GetLastError()));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(buffer.data(), length));
}

}  // namespace

clonecore::Result<WindowsShrinkCaptureExecutor>
make_windows_ntfs_shrink_capture_executor(
    WindowsNtfsShrinkCapturePlan plan,
    WindowsNtfsShrinkCaptureDependencies dependencies) {
  auto bindings = validate_plan(plan, dependencies);
  if (!bindings) {
    return clonecore::Result<WindowsShrinkCaptureExecutor>::failure(
        bindings.error());
  }
  WindowsShrinkCaptureExecutor executor =
      [plan = std::move(plan),
       bindings = bindings.take_value(),
       dependencies = std::move(dependencies)](
          const vssrequester::SnapshotCopyContext& context,
          const std::span<const imageformat::TsumugiManifestPartition>
              partitions,
          const WindowsShrinkWorkPaths& work_paths,
          const clonecore::DiskOperationCallbacks& callbacks) {
        return capture_snapshot_payloads(
            plan,
            bindings,
            dependencies,
            context,
            partitions,
            work_paths,
            callbacks);
      };
  return clonecore::Result<WindowsShrinkCaptureExecutor>::success(
      std::move(executor));
}

clonecore::Result<WindowsShrinkCaptureExecutor>
make_windows_ntfs_shrink_capture_executor_with_windows_apis(
    WindowsNtfsShrinkCapturePlan plan) {
  WindowsNtfsShrinkCaptureDependencies dependencies{
      .open_read_only_source =
          [](const clonecore::StableDiskIdentity& source) {
            return diskmodel::
                open_verified_read_only_physical_disk_with_windows_apis(
                    source);
          },
      .resolve_snapshot_volumes =
          [](const diskmodel::DiskInfo& source,
             const std::span<const diskmodel::VolumePartitionLocation>
                 partitions) {
            return diskmodel::query_windows_volume_bindings_by_offset(
                source, partitions);
          },
      .create_wim_staging =
          [](const std::wstring& scratch) {
            return WindowsNtfsShrinkWimStaging::create(scratch);
          },
      .capture_wim =
          [](const windowsdism::DismCaptureRequest& request) {
            auto system_directory = query_system_directory();
            if (!system_directory) {
              return clonecore::Result<
                  windowsdism::DismExecutionReport>::failure(
                  system_directory.error());
            }
            auto trust = bootrepair::make_windows_authenticode_verifier();
            auto process = bootrepair::make_windows_process_runner(
                kDismTimeoutMilliseconds);
            if (!trust || !process) {
              return failure<windowsdism::DismExecutionReport>(
                  clonecore::ErrorCode::internal_error,
                  ERROR_INVALID_HANDLE,
                  L"Windows縮小WIM DISM依存",
                  L"Authenticode verifierまたはprocess runnerを作成できません");
            }
            return windowsdism::execute_dism_capture(
                request,
                system_directory.value(),
                *trust,
                *process);
          },
  };
  return make_windows_ntfs_shrink_capture_executor(
      std::move(plan), std::move(dependencies));
}

}  // namespace ytec::windowsapp

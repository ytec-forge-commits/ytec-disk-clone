#include "ytec/windowsapp/online_image_create.h"

#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/vssrequester/snapshot_metadata.h"
#include "ytec/vssrequester/snapshot_plan.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

clonecore::Error image_error(
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
  return clonecore::Result<T>::failure(image_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
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

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

bool can_length_prefix(const std::size_t length) noexcept {
  return length <= std::numeric_limits<std::uint32_t>::max();
}

clonecore::Status append_ascii(
    std::vector<std::byte>& bytes,
    const std::string_view value,
    const std::wstring_view operation) {
  if (!can_length_prefix(value.size())) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が識別Hashの上限を超えています"));
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
  return clonecore::success_status();
}

clonecore::Status append_utf16(
    std::vector<std::byte>& bytes,
    const std::wstring_view value,
    const std::wstring_view operation) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (!can_length_prefix(value.size())) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が識別Hashの上限を超えています"));
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t code_unit : value) {
    const auto value16 = static_cast<std::uint16_t>(code_unit);
    bytes.push_back(static_cast<std::byte>(value16 & 0xffU));
    bytes.push_back(static_cast<std::byte>((value16 >> 8U) & 0xffU));
  }
  return clonecore::success_status();
}

void append_domain(
    std::vector<std::byte>& bytes,
    const std::string_view domain) {
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  bytes.push_back(std::byte{0});
}

clonecore::Result<std::string> utf16_to_utf8(
    const std::wstring_view value) {
  if (value.empty()) {
    return clonecore::Result<std::string>::success({});
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Tsumugiパーティション名変換",
        L"パーティション名が長すぎます");
  }
  const int length = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (length <= 0) {
    return clonecore::Result<std::string>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_data,
            L"Tsumugiパーティション名変換",
            GetLastError()));
  }
  std::string converted(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          converted.data(),
          length,
          nullptr,
          nullptr) != length) {
    return clonecore::Result<std::string>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_data,
            L"Tsumugiパーティション名変換",
            GetLastError()));
  }
  return clonecore::Result<std::string>::success(std::move(converted));
}

bool ends_with_tsumugi(const std::wstring_view path) noexcept {
  constexpr std::wstring_view extension = L".tsumugi";
  return path.size() > extension.size() &&
      _wcsnicmp(
          path.data() + path.size() - extension.size(),
          extension.data(),
      extension.size()) == 0;
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      left.type == right.type && left.identifier == right.identifier &&
      left.name == right.name && left.bootable == right.bootable;
}

bool same_reviewed_layout(
    const diskmodel::DiskInfo& reviewed,
    const diskmodel::DiskInfo& observed) {
  if (reviewed.physical_sector_size != observed.physical_sector_size ||
      reviewed.partition_style != observed.partition_style ||
      reviewed.partitions.size() != observed.partitions.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < reviewed.partitions.size(); ++index) {
    if (!same_partition(reviewed.partitions[index], observed.partitions[index])) {
      return false;
    }
  }
  return true;
}

clonecore::Result<const diskmodel::PartitionInfo*>
find_observed_partition_by_range(
    const diskmodel::DiskInfo& source,
    const std::uint64_t offset,
    const std::uint64_t length,
    const std::wstring_view operation) {
  const diskmodel::PartitionInfo* found = nullptr;
  for (const auto& partition : source.partitions) {
    if (partition.offset_bytes != offset || partition.size_bytes != length) {
      continue;
    }
    if (found != nullptr) {
      return failure<const diskmodel::PartitionInfo*>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          std::wstring(operation),
          L"同じ範囲へ複数のPartitionNumberが対応しています");
    }
    found = &partition;
  }
  if (found == nullptr) {
    return failure<const diskmodel::PartitionInfo*>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        std::wstring(operation),
        L"現在のレイアウトに対応するPartitionNumberがありません");
  }
  return clonecore::Result<const diskmodel::PartitionInfo*>::success(found);
}

bool partition_required(
    const diskmodel::ImagePartitionSelection& selection,
    const std::uint32_t partition_number) noexcept {
  return std::find(
             selection.required_partition_numbers.begin(),
             selection.required_partition_numbers.end(),
             partition_number) != selection.required_partition_numbers.end();
}

bool same_image_partition_selection(
    const diskmodel::ImagePartitionSelection& left,
    const diskmodel::ImagePartitionSelection& right) noexcept {
  return left.whole_disk == right.whole_disk &&
      left.contains_windows == right.contains_windows &&
      left.selected_bytes == right.selected_bytes &&
      left.selected_partition_numbers == right.selected_partition_numbers &&
      left.required_partition_numbers == right.required_partition_numbers;
}

bool manifest_partition_selected(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

clonecore::Result<std::vector<std::uint32_t>>
selected_manifest_entry_indices(
    const imageformat::TsumugiManifest& manifest) {
  std::vector<std::uint32_t> result;
  result.reserve(manifest.partitions.size());
  for (const auto& partition : manifest.partitions) {
    if (!manifest_partition_selected(partition)) {
      continue;
    }
    if (partition.source_table_index == 0U) {
      return failure<std::vector<std::uint32_t>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オンラインTsumugi partition選択index",
          L"選択されたmanifestに0のtable indexがあります");
    }
    result.push_back(partition.source_table_index - 1U);
  }
  std::sort(result.begin(), result.end());
  if (result.empty() ||
      std::adjacent_find(result.begin(), result.end()) != result.end()) {
    return failure<std::vector<std::uint32_t>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"オンラインTsumugi partition選択index",
        L"選択されたtable indexが空または重複しています");
  }
  return clonecore::Result<std::vector<std::uint32_t>>::success(
      std::move(result));
}

clonecore::Result<std::vector<clonecore::VolumeBitmapBinding>>
query_selected_gpt_volume_bindings(
    const diskmodel::DiskInfo& disk,
    const clonecore::GptDisk& layout,
    const std::span<const std::uint32_t> selected_entries) {
  std::vector<diskmodel::VolumePartitionLocation> locations;
  for (const auto& partition : layout.partitions) {
    if (partition.type_guid != clonecore::gpt_type_basic_data() ||
        !std::binary_search(
            selected_entries.begin(),
            selected_entries.end(),
            partition.entry_index)) {
      continue;
    }
    std::uint64_t offset{};
    if (!checked_multiply(
            partition.first_lba, layout.logical_sector_size, offset)) {
      return failure<std::vector<clonecore::VolumeBitmapBinding>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"オンラインTsumugi GPT選択Volume範囲",
          L"選択したGPT Basic Dataの開始位置が64bit上限を超えます");
    }
    locations.push_back({
        .table_index = partition.entry_index,
        .offset_bytes = offset,
    });
  }
  if (locations.empty()) {
    return failure<std::vector<clonecore::VolumeBitmapBinding>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンラインTsumugi選択Volume",
        L"Windows版では少なくとも1つのNTFS Basic Data領域を選択してください。静的領域だけの作成はWinPE版を使用してください");
  }
  return diskmodel::query_windows_volume_bindings_by_offset(disk, locations);
}

clonecore::Result<std::vector<clonecore::VolumeBitmapBinding>>
query_selected_mbr_volume_bindings(
    const diskmodel::DiskInfo& disk,
    const clonecore::MbrDisk& layout,
    const std::span<const std::uint32_t> selected_entries) {
  std::vector<diskmodel::VolumePartitionLocation> locations;
  for (const auto& partition : layout.partitions) {
    if (partition.type != 0x07U ||
        !std::binary_search(
            selected_entries.begin(),
            selected_entries.end(),
            static_cast<std::uint32_t>(partition.table_index))) {
      continue;
    }
    std::uint64_t offset{};
    if (!checked_multiply(
            partition.first_lba, layout.logical_sector_size, offset)) {
      return failure<std::vector<clonecore::VolumeBitmapBinding>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"オンラインTsumugi MBR選択Volume範囲",
          L"選択したMBR 0x07領域の開始位置が64bit上限を超えます");
    }
    locations.push_back({
        .table_index = static_cast<std::uint32_t>(partition.table_index),
        .offset_bytes = offset,
    });
  }
  if (locations.empty()) {
    return failure<std::vector<clonecore::VolumeBitmapBinding>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンラインTsumugi選択Volume",
        L"Windows版では少なくとも1つのNTFS 0x07領域を選択してください。静的領域だけの作成はWinPE版を使用してください");
  }
  return diskmodel::query_windows_volume_bindings_by_offset(disk, locations);
}

clonecore::Status validate_request(
    const OnlineImageCreateRequest& request,
    const OnlineImageCreateDependencies& dependencies) {
  if (!request.administrator) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"オンラインTsumugi 管理者確認",
        L"Windows上の直接イメージ作成には管理者権限が必要です。"
        L"この処理からUAC昇格は要求しません"));
  }
  if (!ends_with_tsumugi(request.final_path) || request.created_utc.empty() ||
      request.app_version.empty() || request.windows_architecture.empty() ||
      !imageformat::is_supported_tsumugi_create_verification_mode(
          request.verification_mode)) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンラインTsumugi 作成要求",
        L"保存先は絶対.tsumugiパスとし、作成日時、アプリ版、Windows情報を指定してください"));
  }
  const auto selection = diskmodel::normalize_image_partition_selection(
      request.selected_source,
      request.selected_partition_numbers);
  if (!selection) {
    return clonecore::Status::failure(selection.error());
  }
  if (!dependencies.open_read_only_disk ||
      !dependencies.query_gpt_bindings ||
      !dependencies.query_mbr_bindings ||
      !dependencies.query_destination_file_system ||
      !dependencies.validate_destination ||
      !dependencies.execute_backup) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンラインTsumugi 依存境界",
        L"読取り専用Source、Volume、保存先、またはVSS実行の境界がありません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_data_rescue_request(
    const OnlineImageCreateRequest& request,
    const WindowsDataRescueImageCreateDependencies& dependencies) {
  if (!request.administrator) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"Windowsデータ救出Tsumugi 管理者確認",
        L"Windows上のデータ救出イメージ作成には管理者権限が必要です。この処理からUAC昇格は要求しません"));
  }
  const auto& source = request.selected_source;
  const bool source_preprotected = source.read_only.has_value() &&
          source.offline.has_value() &&
      (source.read_only.value() || source.offline.value());
  if (!ends_with_tsumugi(request.final_path) || request.created_utc.empty() ||
      request.app_version.empty() || request.windows_architecture.empty() ||
      !imageformat::is_supported_tsumugi_create_verification_mode(
          request.verification_mode) ||
      !request.selected_partition_numbers.empty() ||
      source.is_system_disk || source.size_bytes == 0U ||
      source.logical_sector_size != 512U ||
      !imageformat::is_supported_sector_size_pair(
          source.logical_sector_size, source.physical_sector_size) ||
      !source_preprotected ||
      !source.removable.has_value() || source.removable.value() ||
      source.bus_type.empty() ||
      (source.partition_style != diskmodel::PartitionStyle::gpt &&
       source.partition_style != diskmodel::PartitionStyle::mbr)) {
    return clonecore::Status::failure(image_error(
        source.is_system_disk ? clonecore::ErrorCode::unsupported_layout
                              : clonecore::ErrorCode::invalid_argument,
        source.is_system_disk ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER,
        L"Windowsデータ救出Tsumugi 作成要求",
        source.is_system_disk
            ? L"稼働中Windowsのシステムディスク救出はPE版を使用してください"
            : L"既にread-onlyまたはofflineの非removable 512バイトGPT/MBRデータディスク、ディスク全体、絶対.tsumugiパス、作成日時、アプリ版、Windows情報が必要です"));
  }
  if (!dependencies.open_read_only_disk ||
      !dependencies.query_destination_file_system ||
      !dependencies.validate_destination ||
      !dependencies.make_rescue_staging) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windowsデータ救出Tsumugi 依存境界",
        L"読取り専用Source、保存先検証、または所有一時領域の境界がありません"));
  }
  return clonecore::success_status();
}

vssrequester::SnapshotMetadataContext metadata_context(
    const OnlineImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source) {
  return vssrequester::SnapshotMetadataContext{
      .source = source.observed.identity,
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .windows_major = request.windows_major,
      .windows_minor = request.windows_minor,
      .windows_build = request.windows_build,
      .windows_architecture = request.windows_architecture,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
  };
}

vssrequester::SnapshotImagePlanOptions plan_options(
    const OnlineImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    std::vector<std::byte> legacy_manifest,
    std::vector<std::byte> partition_snapshot,
    std::vector<std::uint32_t> selected_partition_entry_indices) {
  return vssrequester::SnapshotImagePlanOptions{
      .administrator = request.administrator,
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .chunk_size = imageformat::kImageChunkSize16MiB,
      .compression = imageformat::ImageCompression::zstandard,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .manifest = std::move(legacy_manifest),
      .partition_table_snapshot = std::move(partition_snapshot),
      .selected_partition_entry_indices =
          std::move(selected_partition_entry_indices),
  };
}

imageformat::TsumugiManifestPartitionRole convert_role(
    const imageformat::BackupPartitionRole role) noexcept {
  using Legacy = imageformat::BackupPartitionRole;
  using Current = imageformat::TsumugiManifestPartitionRole;
  switch (role) {
    case Legacy::efi_system:
      return Current::efi_system;
    case Legacy::microsoft_reserved:
      return Current::microsoft_reserved;
    case Legacy::windows_ntfs:
      return Current::windows;
    case Legacy::recovery_ntfs:
      return Current::recovery;
    case Legacy::fat32_data:
    case Legacy::ntfs_data:
      return Current::data;
  }
  return Current::other;
}

imageformat::TsumugiManifestFileSystem convert_file_system(
    const imageformat::BackupFileSystem file_system) noexcept {
  switch (file_system) {
    case imageformat::BackupFileSystem::none:
      return imageformat::TsumugiManifestFileSystem::none;
    case imageformat::BackupFileSystem::ntfs:
      return imageformat::TsumugiManifestFileSystem::ntfs;
    case imageformat::BackupFileSystem::fat32:
      return imageformat::TsumugiManifestFileSystem::fat32;
  }
  return imageformat::TsumugiManifestFileSystem::unknown;
}

clonecore::Status reject_mutable_raw_data(
    const imageformat::TsumugiManifest& manifest) {
  const auto mutable_partition = std::find_if(
      manifest.partitions.begin(),
      manifest.partitions.end(),
      [](const auto& item) {
        return manifest_partition_selected(item) &&
            item.role == imageformat::TsumugiManifestPartitionRole::data &&
            (item.file_system ==
                 imageformat::TsumugiManifestFileSystem::fat32 ||
             item.file_system ==
                 imageformat::TsumugiManifestFileSystem::exfat);
      });
  if (mutable_partition != manifest.partitions.end()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンラインTsumugi 可変RAWデータ領域",
        L"FAT32／exFATのデータ領域は稼働中Windowsから整合したRAW読取りを保証できません。WinPEで直接イメージを作成してください"));
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::TsumugiManifest> base_manifest(
    const OnlineImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const imageformat::BackupImageManifest& legacy,
    const diskmodel::ImagePartitionSelection& selection,
    const std::span<const std::byte> partition_snapshot,
    const TsumugiSourceIdentityHashes& hashes) {
  imageformat::TsumugiManifest result{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style =
          legacy.partition_style == imageformat::BackupPartitionStyle::gpt
          ? imageformat::TsumugiManifestPartitionStyle::gpt
          : imageformat::TsumugiManifestPartitionStyle::mbr,
      .flags =
          (selection.contains_windows
               ? imageformat::TsumugiManifestFlags::source_contains_windows
               : imageformat::TsumugiManifestFlags::none) |
          (!selection.whole_disk
               ? imageformat::TsumugiManifestFlags::partition_selection
               : imageformat::TsumugiManifestFlags::none),
      .source_disk_size = source.reader->size_bytes(),
      .logical_sector_size = source.reader->logical_sector_size(),
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .source_model_hash = hashes.model,
      .source_serial_hash = hashes.serial,
      .source_state_hash = hashes.locked_state,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
      .partition_snapshot = std::vector<std::byte>(
          partition_snapshot.begin(), partition_snapshot.end()),
  };
  result.partitions.reserve(legacy.partitions.size());
  return clonecore::Result<imageformat::TsumugiManifest>::success(
      std::move(result));
}

clonecore::Result<imageformat::TsumugiManifest> convert_gpt_manifest(
    const OnlineImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const imageformat::BackupImageManifest& legacy,
    const clonecore::GptDisk& layout,
    const diskmodel::ImagePartitionSelection& selection,
    const std::span<const std::byte> partition_snapshot,
    const TsumugiSourceIdentityHashes& hashes) {
  auto converted = base_manifest(
      request, source, legacy, selection, partition_snapshot, hashes);
  if (!converted) {
    return converted;
  }
  for (const auto& record : legacy.partitions) {
    const auto layout_partition = std::find_if(
        layout.partitions.begin(), layout.partitions.end(),
        [&](const auto& item) { return item.entry_index == record.table_index; });
    if (layout_partition == layout.partitions.end() ||
        layout_partition->last_lba < layout_partition->first_lba ||
        layout_partition->first_lba * layout.logical_sector_size !=
            record.offset_bytes ||
        (layout_partition->last_lba - layout_partition->first_lba + 1U) *
                layout.logical_sector_size !=
            record.length_bytes ||
        record.table_index == std::numeric_limits<std::uint32_t>::max()) {
      return failure<imageformat::TsumugiManifest>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"オンラインTsumugi GPTマニフェスト変換",
          L"GPTレコードと検証済み旧メタデータの範囲が一致しません");
    }
    auto name = utf16_to_utf8(record.name);
    if (!name) {
      return clonecore::Result<imageformat::TsumugiManifest>::failure(
          name.error());
    }
    auto observed_partition = find_observed_partition_by_range(
        source.observed.observed,
        record.offset_bytes,
        record.length_bytes,
        L"オンラインTsumugi GPT PartitionNumber対応");
    if (!observed_partition) {
      return clonecore::Result<imageformat::TsumugiManifest>::failure(
          observed_partition.error());
    }
    const auto partition_number = observed_partition.value()->number;
    const bool selected = diskmodel::image_partition_selection_contains(
        selection, partition_number);
    const bool required = selected &&
        partition_required(selection, partition_number);
    auto flags = selected
        ? imageformat::TsumugiManifestPartitionFlags::selected
        : imageformat::TsumugiManifestPartitionFlags::none;
    if (required) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::required;
    }
    const bool contains_windows = selected && required &&
        layout_partition->type_guid == clonecore::gpt_type_basic_data();
    if (contains_windows) {
      flags = flags |
          imageformat::TsumugiManifestPartitionFlags::contains_windows;
    }
    imageformat::TsumugiManifestPartition partition{
        .source_table_index = record.table_index + 1U,
        .source_partition_number = partition_number,
        .role = contains_windows
            ? imageformat::TsumugiManifestPartitionRole::windows
            : convert_role(record.role),
        .file_system = convert_file_system(record.file_system),
        .flags = flags,
        .source_offset = record.offset_bytes,
        .source_size = record.length_bytes,
        .used_bytes = selected &&
                record.role !=
                    imageformat::BackupPartitionRole::microsoft_reserved
            ? record.length_bytes
            : 0U,
        .minimum_target_bytes = selected ? record.length_bytes : 0U,
        .planned_target_bytes = selected ? record.length_bytes : 0U,
        .payload_logical_offset = selected ? record.offset_bytes : 0U,
        .payload_logical_length = selected ? record.length_bytes : 0U,
        .type_id = layout_partition->type_guid.bytes,
        .unique_id = layout_partition->unique_guid.bytes,
        .name_utf8 = name.take_value(),
    };
    converted.value().partitions.push_back(std::move(partition));
  }
  return converted;
}

clonecore::Result<imageformat::TsumugiManifest> convert_mbr_manifest(
    const OnlineImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const imageformat::BackupImageManifest& legacy,
    const clonecore::MbrDisk& layout,
    const diskmodel::ImagePartitionSelection& selection,
    const std::span<const std::byte> partition_snapshot,
    const TsumugiSourceIdentityHashes& hashes) {
  auto converted = base_manifest(
      request, source, legacy, selection, partition_snapshot, hashes);
  if (!converted) {
    return converted;
  }
  for (const auto& record : legacy.partitions) {
    const auto layout_partition = std::find_if(
        layout.partitions.begin(), layout.partitions.end(),
        [&](const auto& item) {
          return static_cast<std::uint32_t>(item.table_index) ==
              record.table_index;
        });
    if (layout_partition == layout.partitions.end() ||
        static_cast<std::uint64_t>(layout_partition->first_lba) *
                layout.logical_sector_size !=
            record.offset_bytes ||
        static_cast<std::uint64_t>(layout_partition->sector_count) *
                layout.logical_sector_size !=
            record.length_bytes ||
        record.table_index == std::numeric_limits<std::uint32_t>::max()) {
      return failure<imageformat::TsumugiManifest>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"オンラインTsumugi MBRマニフェスト変換",
          L"MBRレコードと検証済み旧メタデータの範囲が一致しません");
    }
    auto name = utf16_to_utf8(record.name);
    if (!name) {
      return clonecore::Result<imageformat::TsumugiManifest>::failure(
          name.error());
    }
    auto observed_partition = find_observed_partition_by_range(
        source.observed.observed,
        record.offset_bytes,
        record.length_bytes,
        L"オンラインTsumugi MBR PartitionNumber対応");
    if (!observed_partition) {
      return clonecore::Result<imageformat::TsumugiManifest>::failure(
          observed_partition.error());
    }
    const auto partition_number = observed_partition.value()->number;
    const bool selected = diskmodel::image_partition_selection_contains(
        selection, partition_number);
    const bool required = selected &&
        partition_required(selection, partition_number);
    auto flags = selected
        ? imageformat::TsumugiManifestPartitionFlags::selected
        : imageformat::TsumugiManifestPartitionFlags::none;
    if (required) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::required;
    }
    if (layout_partition->active) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::active;
    }
    const bool contains_windows = selected && required &&
        layout_partition->type == 0x07U;
    if (contains_windows) {
      flags = flags |
          imageformat::TsumugiManifestPartitionFlags::contains_windows;
    }
    imageformat::TsumugiManifestPartition partition{
        .source_table_index = record.table_index + 1U,
        .source_partition_number = partition_number,
        .role = contains_windows
            ? imageformat::TsumugiManifestPartitionRole::windows
            : convert_role(record.role),
        .file_system = convert_file_system(record.file_system),
        .flags = flags,
        .source_offset = record.offset_bytes,
        .source_size = record.length_bytes,
        .used_bytes = selected ? record.length_bytes : 0U,
        .minimum_target_bytes = selected ? record.length_bytes : 0U,
        .planned_target_bytes = selected ? record.length_bytes : 0U,
        .payload_logical_offset = selected ? record.offset_bytes : 0U,
        .payload_logical_length = selected ? record.length_bytes : 0U,
        .name_utf8 = name.take_value(),
    };
    partition.type_id[0] = static_cast<std::byte>(layout_partition->type);
    converted.value().partitions.push_back(std::move(partition));
  }
  return converted;
}

clonecore::Result<vssrequester::TsumugiSnapshotImageRequest>
convert_snapshot_plan(
    const OnlineImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    vssrequester::PreparedSnapshotImagePlan legacy_plan,
    imageformat::TsumugiManifest manifest,
    const TsumugiSourceIdentityHashes& hashes,
    const imageformat::TsumugiImageStorageFileSystem storage_file_system,
    std::shared_ptr<std::uint64_t> required_bytes,
    OnlineImageDestinationValidator destination_validator) {
  if (legacy_plan.workflow.volumes.size() !=
      legacy_plan.image_copy.volumes.size()) {
    return failure<vssrequester::TsumugiSnapshotImageRequest>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"オンラインTsumugi Volume変換",
        L"VSS Workflowと画像Volumeの件数が一致しません");
  }

  vssrequester::TsumugiSnapshotImageRequest result;
  result.image = imageformat::TsumugiImageCreateRequest{
      .final_path = request.final_path,
      .storage_file_system = storage_file_system,
      .manifest = std::move(manifest),
      .compression = imageformat::ImageCompression::zstandard,
      .chunk_size = imageformat::kImageChunkSize16MiB,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .verification_mode = request.verification_mode,
      .replace_existing = request.replace_existing,
  };
  if (request.encryption_password.has_value()) {
    result.image.encryption = imageformat::TsumugiImageEncryptionRequest{
        .password = *request.encryption_password,
    };
  }
  result.volumes.reserve(legacy_plan.image_copy.volumes.size());
  for (std::size_t index = 0U;
       index < legacy_plan.image_copy.volumes.size(); ++index) {
    const auto& volume = legacy_plan.image_copy.volumes[index];
    if (volume.partition_entry_index ==
        std::numeric_limits<std::uint32_t>::max()) {
      return failure<vssrequester::TsumugiSnapshotImageRequest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"オンラインTsumugi Volume番号",
          L"パーティション番号を正規化できません");
    }
    const auto manifest_partition = std::find_if(
        result.image.manifest.partitions.begin(),
        result.image.manifest.partitions.end(),
        [&](const auto& item) {
          return item.source_table_index ==
                  volume.partition_entry_index + 1U &&
              item.source_offset == volume.disk_offset &&
              item.source_size == volume.partition_length;
        });
    if (manifest_partition == result.image.manifest.partitions.end() ||
        !manifest_partition_selected(*manifest_partition)) {
      return failure<vssrequester::TsumugiSnapshotImageRequest>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"オンラインTsumugi Volume選択binding",
          L"VSS Volume計画が選択済みmanifest領域と一致しません");
    }
    result.volumes.push_back(vssrequester::TsumugiSnapshotVolumePlan{
        .partition_entry_index = volume.partition_entry_index + 1U,
        .disk_offset = volume.disk_offset,
        .partition_length = volume.partition_length,
        .original_volume_guid_path =
            legacy_plan.workflow.volumes[index].volume_guid_path,
    });
  }

  result.raw_regions.reserve(legacy_plan.image_copy.raw_regions.size());
  for (const auto& raw : legacy_plan.image_copy.raw_regions) {
    const auto partition = std::find_if(
        result.image.manifest.partitions.begin(),
        result.image.manifest.partitions.end(),
        [&](const auto& item) {
          return item.source_offset == raw.disk_offset &&
              item.source_size == raw.length;
        });
    if (partition == result.image.manifest.partitions.end() ||
        !manifest_partition_selected(*partition) ||
        (partition->role !=
             imageformat::TsumugiManifestPartitionRole::efi_system &&
         partition->role !=
             imageformat::TsumugiManifestPartitionRole::recovery)) {
      return failure<vssrequester::TsumugiSnapshotImageRequest>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンラインTsumugi 固定RAW領域",
          L"稼働中WindowsではESP／回復領域以外を物理SourceからRAW読取りできません");
    }
    result.raw_regions.push_back(vssrequester::TsumugiSnapshotRawRegion{
        .partition_entry_index = partition->source_table_index,
        .disk_offset = raw.disk_offset,
        .length = raw.length,
        .source_offset = raw.source_offset,
    });
  }
  result.locked_raw_source = source.reader.get();
  result.locked_source_state_hash = hashes.locked_state;
  const auto expected_snapshot = result.image.manifest.partition_snapshot;
  const auto expected_style = result.image.manifest.partition_style;
  const auto* reader = source.reader.get();
  result.revalidate_locked_layout =
      [reader, expected_snapshot, expected_style]() {
        const auto observed = imageformat::capture_partition_snapshot_v1(
            *reader,
            expected_style == imageformat::TsumugiManifestPartitionStyle::gpt
                ? imageformat::PartitionTableStyle::gpt
                : imageformat::PartitionTableStyle::mbr);
        if (!observed) {
          return clonecore::Status::failure(observed.error());
        }
        if (observed.value() != expected_snapshot) {
          return clonecore::Status::failure(image_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_CRC,
              L"オンラインTsumugi パーティション表再確認",
              L"ロック済み読取り専用Sourceのパーティション表が開始時点から変化しました"));
        }
        return clonecore::success_status();
      };

  const imageformat::WindowsTsumugiDestinationGuardRequest guard{
      .final_path = request.final_path,
      .expected_source_disk = source.observed.identity,
      .required_available_bytes = 1U,
      .replace_existing = request.replace_existing,
  };
  result.validate_destination_capacity =
      [required_bytes = std::move(required_bytes),
       destination_validator = std::move(destination_validator),
       guard](const std::uint64_t bytes) mutable {
        if (bytes == 0U) {
          return clonecore::Status::failure(image_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"オンラインTsumugi 保存容量",
              L"必要保存容量が0です"));
        }
        *required_bytes = bytes;
        auto current = guard;
        current.required_available_bytes = bytes;
        return destination_validator(current);
      };
  return clonecore::Result<vssrequester::TsumugiSnapshotImageRequest>::success(
      std::move(result));
}

clonecore::Result<imageformat::TsumugiImageStorageFileSystem>
query_destination_file_system_with_windows_apis(const std::wstring& path) {
  // Keep the long-path buffer off the stack. This preserves the fixed bound
  // and API behavior while avoiding a 64 KiB frame in /analyze builds.
  std::vector<wchar_t> full(32768U, L'\0');
  const DWORD length = GetFullPathNameW(
      path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先絶対パス取得",
            length == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  std::array<wchar_t, MAX_PATH + 1U> root{};
  if (!GetVolumePathNameW(
          full.data(), root.data(), static_cast<DWORD>(root.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先Volume取得",
            GetLastError()));
  }
  std::array<wchar_t, 32U> file_system{};
  if (!GetVolumeInformationW(
          root.data(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi保存先ファイルシステム取得",
            GetLastError()));
  }
  if (_wcsicmp(file_system.data(), L"NTFS") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::ntfs);
  }
  if (_wcsicmp(file_system.data(), L"exFAT") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::exfat);
  }
  return failure<imageformat::TsumugiImageStorageFileSystem>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"Tsumugi保存先ファイルシステム",
      L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
}

class WindowsDataRescueSourceSession final
    : public imageformat::ITsumugiImageSourceSession {
 public:
  WindowsDataRescueSourceSession(
      std::unique_ptr<clonecore::ISourceDiskReader> reader,
      TsumugiSourceIdentityHashes hashes) noexcept
      : reader_(std::move(reader)), hashes_(std::move(hashes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return reader_ ? reader_->size_bytes() : 0U;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return reader_ ? reader_->logical_sector_size() : 0U;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!reader_) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_HANDLE,
          L"Windowsデータ救出Tsumugi Source読取り",
          L"読取り専用Source sessionがありません");
    }
    return reader_->read(offset, length);
  }

  [[nodiscard]] imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    return hashes_.model;
  }

  [[nodiscard]] imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    return hashes_.serial;
  }

  [[nodiscard]] imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    return hashes_.locked_state;
  }

 private:
  std::unique_ptr<clonecore::ISourceDiskReader> reader_;
  TsumugiSourceIdentityHashes hashes_;
};

struct RescueImageSizing final {
  std::uint64_t logical_payload_bytes{};
  std::uint64_t maximum_image_bytes{};
};

bool manifest_partition_selected(
    const imageformat::TsumugiManifestPartitionFlags flags) noexcept {
  return (static_cast<std::uint32_t>(flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

clonecore::Result<RescueImageSizing> rescue_image_sizing(
    const imageformat::TsumugiManifest& manifest) {
  auto encoded = imageformat::build_tsumugi_manifest_v1(manifest);
  if (!encoded) {
    return clonecore::Result<RescueImageSizing>::failure(encoded.error());
  }
  RescueImageSizing result;
  for (const auto& partition : manifest.partitions) {
    if (manifest_partition_selected(partition.flags) &&
        !checked_add(
            result.logical_payload_bytes,
            partition.payload_logical_length,
            result.logical_payload_bytes)) {
      return failure<RescueImageSizing>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Windowsデータ救出Tsumugi payload寸法",
          L"選択payload合計が64bit上限を超えます");
    }
  }
  std::uint64_t records{};
  std::uint64_t total = imageformat::kTsumugiHeaderSize;
  if (result.logical_payload_bytes == 0U ||
      !checked_multiply(
          imageformat::kTsumugiMaximumChunkCount,
          imageformat::kTsumugiChunkRecordSize,
          records) ||
      !checked_add(total, imageformat::kTsumugiMetadataHeaderSize, total) ||
      !checked_add(total, encoded.value().size(), total) ||
      !checked_add(total, records, total) ||
      !checked_add(total, result.logical_payload_bytes, total) ||
      !checked_add(total, imageformat::kTsumugiFooterSize, total)) {
    return failure<RescueImageSizing>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Windowsデータ救出Tsumugi最大画像寸法",
        L"選択payloadがないか、画像の最大寸法が64bit上限を超えます");
  }
  result.maximum_image_bytes = total;
  return clonecore::Result<RescueImageSizing>::success(result);
}

clonecore::DiskOperationProgress translate_rescue_progress(
    const clonecore::RescueCopyProgress& progress) {
  clonecore::DiskOperationStage stage =
      clonecore::DiskOperationStage::copying_data;
  if (progress.phase == clonecore::RescueCopyPhase::validating) {
    stage = clonecore::DiskOperationStage::verifying_source;
  } else if (progress.phase == clonecore::RescueCopyPhase::flushing) {
    stage = clonecore::DiskOperationStage::flushing_data;
  }
  return clonecore::DiskOperationProgress{
      .stage = stage,
      .partition_index = std::nullopt,
      .total_read_bytes = progress.source_extent_bytes,
      .total_write_bytes = progress.source_extent_bytes,
      .total_verify_bytes = progress.source_extent_bytes,
      .read_bytes = progress.settled_target_bytes,
      .written_bytes = progress.settled_target_bytes,
      .verified_bytes = progress.settled_target_bytes,
      .cancellation_allowed = progress.cancellation_allowed,
      .pause_allowed = progress.pause_allowed,
  };
}

}  // namespace

clonecore::Result<imageformat::Sha256Digest>
hash_tsumugi_source_model(const std::wstring_view model) {
  return imageformat::hash_tsumugi_source_model_v1(model);
}

clonecore::Result<imageformat::Sha256Digest>
hash_tsumugi_source_serial(
    const std::string_view serial_suffix,
    const std::wstring_view device_instance_id) {
  return imageformat::hash_tsumugi_source_serial_v1(
      serial_suffix, device_instance_id);
}

clonecore::Result<TsumugiSourceIdentityHashes>
make_tsumugi_source_identity_hashes(
    const clonecore::StableDiskIdentity& source,
    const std::uint32_t physical_sector_size,
    const std::span<const std::byte> canonical_partition_snapshot) {
  const auto identity = clonecore::validate_stable_identity(
      source, source, L"Tsumugiコピー元");
  if (!identity ||
      !imageformat::is_supported_sector_size_pair(
          source.logical_sector_size, physical_sector_size) ||
      canonical_partition_snapshot.empty()) {
    return clonecore::Result<TsumugiSourceIdentityHashes>::failure(
        identity
            ? image_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"Tsumugi Source状態Hash",
                  L"物理セクターまたはパーティションsnapshotがありません")
            : identity.error());
  }
  const auto snapshot = imageformat::inspect_partition_snapshot_v1(
      canonical_partition_snapshot);
  if (!snapshot || snapshot.value().source_disk_size != source.size_bytes ||
      snapshot.value().logical_sector_size != source.logical_sector_size) {
    return clonecore::Result<TsumugiSourceIdentityHashes>::failure(
        snapshot
            ? image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"Tsumugi Source状態Hash",
                  L"安定識別とpartition snapshotの寸法が一致しません")
            : snapshot.error());
  }
  auto model = hash_tsumugi_source_model(source.model);
  if (!model) {
    return clonecore::Result<TsumugiSourceIdentityHashes>::failure(
        model.error());
  }
  auto serial = hash_tsumugi_source_serial(
      source.serial_suffix, source.device_instance_id);
  if (!serial) {
    return clonecore::Result<TsumugiSourceIdentityHashes>::failure(
        serial.error());
  }

  std::vector<std::byte> material;
  material.reserve(128U + canonical_partition_snapshot.size());
  append_domain(material, "YTEC-TSUMUGI-LOCKED-SOURCE-STATE-V1");
  const auto model_appended = append_utf16(
      material, source.model, L"Tsumugi Source状態Hash");
  const auto serial_appended = append_ascii(
      material, source.serial_suffix, L"Tsumugi Source状態Hash");
  const auto instance_appended = append_utf16(
      material, source.device_instance_id, L"Tsumugi Source状態Hash");
  if (!model_appended || !serial_appended || !instance_appended) {
    return clonecore::Result<TsumugiSourceIdentityHashes>::failure(
        !model_appended
            ? model_appended.error()
            : !serial_appended ? serial_appended.error()
                               : instance_appended.error());
  }
  append_u64(material, source.size_bytes);
  append_u32(material, source.logical_sector_size);
  append_u32(material, physical_sector_size);
  material.push_back(source.is_system_disk ? std::byte{1} : std::byte{0});
  append_u64(
      material,
      static_cast<std::uint64_t>(canonical_partition_snapshot.size()));
  material.insert(
      material.end(),
      canonical_partition_snapshot.begin(),
      canonical_partition_snapshot.end());
  auto state = imageformat::sha256(material);
  if (!state) {
    return clonecore::Result<TsumugiSourceIdentityHashes>::failure(
        state.error());
  }
  return clonecore::Result<TsumugiSourceIdentityHashes>::success(
      TsumugiSourceIdentityHashes{
          .model = model.take_value(),
          .serial = serial.take_value(),
          .locked_state = state.take_value(),
      });
}

clonecore::Result<vssrequester::OnlineTsumugiBackupReport>
execute_online_image_create(
    const OnlineImageCreateRequest& request,
    const OnlineImageCreateDependencies& dependencies) {
  const auto valid = validate_request(request, dependencies);
  if (!valid) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(valid.error());
  }
  auto reviewed_selection = diskmodel::normalize_image_partition_selection(
      request.selected_source,
      request.selected_partition_numbers);
  if (!reviewed_selection) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        reviewed_selection.error());
  }
  auto expected = diskmodel::make_stable_disk_identity(
      request.selected_source, request.selected_source.is_system_disk);
  if (!expected) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(expected.error());
  }

  const imageformat::WindowsTsumugiDestinationGuardRequest initial_guard{
      .final_path = request.final_path,
      .expected_source_disk = expected.value(),
      .required_available_bytes = 1U,
      .replace_existing = request.replace_existing,
  };
  const auto destination = dependencies.validate_destination(initial_guard);
  if (!destination) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        destination.error());
  }
  auto storage_file_system =
      dependencies.query_destination_file_system(request.final_path);
  if (!storage_file_system) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        storage_file_system.error());
  }
  if (storage_file_system.value() !=
          imageformat::TsumugiImageStorageFileSystem::ntfs &&
      storage_file_system.value() !=
          imageformat::TsumugiImageStorageFileSystem::exfat) {
    return failure<vssrequester::OnlineTsumugiBackupReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi保存先ファイルシステム",
        L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
  }

  auto source = dependencies.open_read_only_disk(expected.value());
  if (!source) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(source.error());
  }
  if (!source.value().reader ||
      !same_reviewed_layout(
          request.selected_source,
          source.value().observed.observed)) {
    return failure<vssrequester::OnlineTsumugiBackupReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"オンラインTsumugi 読取り専用Source再識別",
        L"検証済み読取り専用Readerまたはレビュー済みレイアウトが一致しません");
  }
  auto observed_selection = diskmodel::normalize_image_partition_selection(
      source.value().observed.observed,
      request.selected_partition_numbers);
  if (!observed_selection ||
      !same_image_partition_selection(
          reviewed_selection.value(), observed_selection.value())) {
    return observed_selection
        ? failure<vssrequester::OnlineTsumugiBackupReport>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"オンラインTsumugi partition選択再証明",
              L"レビュー後に選択領域または必須Windows領域のbindingが変化しました")
        : clonecore::Result<
              vssrequester::OnlineTsumugiBackupReport>::failure(
              observed_selection.error());
  }
  const auto source_identity = clonecore::validate_stable_identity(
      expected.value(),
      source.value().observed.identity,
      L"オンラインTsumugi コピー元");
  if (!source_identity) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        source_identity.error());
  }
  clonecore::Result<vssrequester::PreparedSnapshotImagePlan> legacy_plan =
      failure<vssrequester::PreparedSnapshotImagePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンラインTsumugi パーティション形式",
          L"コピー元はGPTまたはMBRでなければなりません");
  clonecore::Result<imageformat::TsumugiManifest> manifest =
      failure<imageformat::TsumugiManifest>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンラインTsumugi マニフェスト",
          L"コピー元の形式を解釈できません");

  if (source.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    auto metadata = vssrequester::build_gpt_snapshot_metadata(
        *source.value().reader, metadata_context(request, source.value()));
    if (!metadata) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(metadata.error());
    }
    auto metadata_value = metadata.take_value();
    auto legacy_manifest = imageformat::inspect_backup_manifest_v1(
        metadata_value.backup_manifest);
    if (!legacy_manifest) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(
          legacy_manifest.error());
    }
    auto hashes = make_tsumugi_source_identity_hashes(
        source.value().observed.identity,
        source.value().observed.observed.physical_sector_size,
        metadata_value.partition_table_snapshot);
    if (!hashes) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(hashes.error());
    }
    manifest = convert_gpt_manifest(
        request,
        source.value(),
        legacy_manifest.value(),
        metadata_value.layout,
        observed_selection.value(),
        metadata_value.partition_table_snapshot,
        hashes.value());
    if (!manifest) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(manifest.error());
    }
    const auto raw_safety = reject_mutable_raw_data(manifest.value());
    auto selected_entries = selected_manifest_entry_indices(manifest.value());
    if (!raw_safety || !selected_entries) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(
          raw_safety ? selected_entries.error() : raw_safety.error());
    }
    auto bindings = dependencies.query_gpt_bindings(
        source.value().observed.observed,
        metadata_value.layout,
        selected_entries.value());
    if (!bindings) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(bindings.error());
    }
    legacy_plan = vssrequester::prepare_gpt_snapshot_image_plan(
        metadata_value.layout,
        *source.value().reader,
        bindings.value(),
        plan_options(
            request,
            source.value(),
            std::move(metadata_value.backup_manifest),
            std::move(metadata_value.partition_table_snapshot),
            selected_entries.take_value()));
  } else if (source.value().observed.observed.partition_style ==
             diskmodel::PartitionStyle::mbr) {
    auto metadata = vssrequester::build_mbr_snapshot_metadata(
        *source.value().reader, metadata_context(request, source.value()));
    if (!metadata) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(metadata.error());
    }
    auto metadata_value = metadata.take_value();
    auto legacy_manifest = imageformat::inspect_backup_manifest_v1(
        metadata_value.backup_manifest);
    if (!legacy_manifest) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(
          legacy_manifest.error());
    }
    auto hashes = make_tsumugi_source_identity_hashes(
        source.value().observed.identity,
        source.value().observed.observed.physical_sector_size,
        metadata_value.partition_table_snapshot);
    if (!hashes) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(hashes.error());
    }
    manifest = convert_mbr_manifest(
        request,
        source.value(),
        legacy_manifest.value(),
        metadata_value.layout,
        observed_selection.value(),
        metadata_value.partition_table_snapshot,
        hashes.value());
    if (!manifest) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(manifest.error());
    }
    const auto raw_safety = reject_mutable_raw_data(manifest.value());
    auto selected_entries = selected_manifest_entry_indices(manifest.value());
    if (!raw_safety || !selected_entries) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(
          raw_safety ? selected_entries.error() : raw_safety.error());
    }
    auto bindings = dependencies.query_mbr_bindings(
        source.value().observed.observed,
        metadata_value.layout,
        selected_entries.value());
    if (!bindings) {
      return clonecore::Result<
          vssrequester::OnlineTsumugiBackupReport>::failure(bindings.error());
    }
    legacy_plan = vssrequester::prepare_mbr_snapshot_image_plan(
        metadata_value.layout,
        *source.value().reader,
        bindings.value(),
        plan_options(
            request,
            source.value(),
            std::move(metadata_value.backup_manifest),
            std::move(metadata_value.partition_table_snapshot),
            selected_entries.take_value()));
  }
  if (!legacy_plan) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        legacy_plan.error());
  }
  const auto canonical_manifest =
      imageformat::build_tsumugi_manifest_v1(manifest.value());
  if (!canonical_manifest) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        canonical_manifest.error());
  }

  auto hashes = make_tsumugi_source_identity_hashes(
      source.value().observed.identity,
      source.value().observed.observed.physical_sector_size,
      manifest.value().partition_snapshot);
  if (!hashes) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(hashes.error());
  }
  auto required_bytes = std::make_shared<std::uint64_t>(1U);
  auto current_plan = convert_snapshot_plan(
      request,
      source.value(),
      legacy_plan.take_value(),
      manifest.take_value(),
      hashes.value(),
      storage_file_system.value(),
      required_bytes,
      dependencies.validate_destination);
  if (!current_plan) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        current_plan.error());
  }

  const auto guard = initial_guard;
  vssrequester::PreparedOnlineTsumugiBackup prepared{
      .workflow = vssrequester::WorkflowRequest{},
      .image = current_plan.take_value(),
      .revalidate_destination =
          [required_bytes,
           validator = dependencies.validate_destination,
           guard](const imageformat::TsumugiImageCreateReport*
                      staged_report) mutable {
            auto current = guard;
            current.required_available_bytes = *required_bytes;
            if (staged_report != nullptr) {
              current.phase = imageformat::
                  WindowsTsumugiDestinationGuardPhase::
                      before_commit_owned_partial;
              current.expected_owned_partial_bytes =
                  staged_report->stream.image_length;
            }
            return validator(current);
          },
  };
  // The workflow was moved into the conversion input; reconstruct it from the
  // image's original Volume GUID order without trusting a second query.
  prepared.workflow.administrator = request.administrator;
  prepared.workflow.volumes.reserve(prepared.image.volumes.size());
  for (const auto& volume : prepared.image.volumes) {
    prepared.workflow.volumes.push_back(vssrequester::VolumeRequest{
        .volume_guid_path = volume.original_volume_guid_path,
        .file_system = L"NTFS",
    });
  }

  return dependencies.execute_backup(
      vssrequester::WindowsOnlineTsumugiBackupRequest{
          .prepared = std::move(prepared),
          .async_wait = request.async_wait,
          .callbacks = request.callbacks,
          .diff_area_review_callback = request.diff_area_review_callback,
          .logger = request.logger,
      });
}

clonecore::Result<vssrequester::OnlineTsumugiBackupReport>
execute_online_image_create_with_windows_apis(
    const OnlineImageCreateRequest& request) {
  return execute_online_image_create(
      request,
      OnlineImageCreateDependencies{
          .open_read_only_disk =
              [](const clonecore::StableDiskIdentity& expected) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        expected);
              },
          .query_gpt_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::GptDisk& layout,
                 const std::span<const std::uint32_t> selected_entries) {
                return query_selected_gpt_volume_bindings(
                    disk, layout, selected_entries);
              },
          .query_mbr_bindings =
              [](const diskmodel::DiskInfo& disk,
                 const clonecore::MbrDisk& layout,
                 const std::span<const std::uint32_t> selected_entries) {
                return query_selected_mbr_volume_bindings(
                    disk, layout, selected_entries);
              },
          .query_destination_file_system =
              query_destination_file_system_with_windows_apis,
          .validate_destination =
              imageformat::validate_windows_tsumugi_destination,
          .execute_backup =
              [](const vssrequester::WindowsOnlineTsumugiBackupRequest&
                     execution) {
                return vssrequester::execute_windows_online_tsumugi_backup(
                    execution);
              },
      });
}

clonecore::Result<WindowsDataRescueImageCreateReport>
execute_windows_data_rescue_image_create(
    const OnlineImageCreateRequest& request,
    const WindowsDataRescueImageCreateDependencies& dependencies) {
  const auto valid = validate_data_rescue_request(request, dependencies);
  if (!valid) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        valid.error());
  }
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return failure<WindowsDataRescueImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"Windowsデータ救出Tsumugi開始前",
        L"開始前に取り消されました");
  }
  auto expected = diskmodel::make_stable_disk_identity(
      request.selected_source, false);
  if (!expected) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        expected.error());
  }

  imageformat::WindowsTsumugiDestinationGuardRequest destination_guard{
      .final_path = request.final_path,
      .expected_source_disk = expected.value(),
      .required_available_bytes = 1U,
      .replace_existing = request.replace_existing,
  };
  auto destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        destination.error());
  }
  auto storage_file_system =
      dependencies.query_destination_file_system(request.final_path);
  if (!storage_file_system) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        storage_file_system.error());
  }
  if (storage_file_system.value() !=
          imageformat::TsumugiImageStorageFileSystem::ntfs &&
      storage_file_system.value() !=
          imageformat::TsumugiImageStorageFileSystem::exfat) {
    return failure<WindowsDataRescueImageCreateReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windowsデータ救出Tsumugi保存先",
        L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
  }

  auto source = dependencies.open_read_only_disk(expected.value());
  if (!source) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        source.error());
  }
  const auto& observed = source.value().observed.observed;
  const bool observed_preprotected = observed.read_only.has_value() &&
          observed.offline.has_value() &&
      (observed.read_only.value() || observed.offline.value());
  if (!source.value().reader || observed.is_system_disk ||
      !observed_preprotected || !observed.removable.has_value() ||
      observed.removable.value() || observed.bus_type.empty() ||
      observed.logical_sector_size != 512U ||
      !imageformat::is_supported_sector_size_pair(
          observed.logical_sector_size, observed.physical_sector_size) ||
      !same_reviewed_layout(request.selected_source, observed)) {
    return failure<WindowsDataRescueImageCreateReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windowsデータ救出Tsumugi Source再識別",
        L"非system、既保護、非removable、512バイトsector、またはレビュー済みレイアウトを再確認できません");
  }
  const auto source_identity = clonecore::validate_stable_identity(
      expected.value(),
      source.value().observed.identity,
      L"Windowsデータ救出Tsumugi Source");
  if (!source_identity) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        source_identity.error());
  }
  auto whole_selection = diskmodel::normalize_image_partition_selection(
      observed,
      std::span<const std::uint32_t>{});
  if (!whole_selection) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        whole_selection.error());
  }

  clonecore::Result<imageformat::TsumugiManifest> manifest =
      failure<imageformat::TsumugiManifest>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windowsデータ救出Tsumugiマニフェスト",
          L"コピー元のGPT/MBR形式を解釈できません");
  clonecore::Result<TsumugiSourceIdentityHashes> hashes =
      failure<TsumugiSourceIdentityHashes>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Windowsデータ救出Tsumugi Source Hash",
          L"コピー元の識別Hashを作成できません");

  if (observed.partition_style == diskmodel::PartitionStyle::gpt) {
    auto metadata = vssrequester::build_gpt_snapshot_metadata(
        *source.value().reader, metadata_context(request, source.value()));
    if (!metadata) {
      return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
          metadata.error());
    }
    auto metadata_value = metadata.take_value();
    auto legacy_manifest = imageformat::inspect_backup_manifest_v1(
        metadata_value.backup_manifest);
    if (!legacy_manifest) {
      return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
          legacy_manifest.error());
    }
    hashes = make_tsumugi_source_identity_hashes(
        source.value().observed.identity,
        observed.physical_sector_size,
        metadata_value.partition_table_snapshot);
    if (!hashes) {
      return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
          hashes.error());
    }
    manifest = convert_gpt_manifest(
        request,
        source.value(),
        legacy_manifest.value(),
        metadata_value.layout,
        whole_selection.value(),
        metadata_value.partition_table_snapshot,
        hashes.value());
  } else {
    auto metadata = vssrequester::build_mbr_snapshot_metadata(
        *source.value().reader, metadata_context(request, source.value()));
    if (!metadata) {
      return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
          metadata.error());
    }
    auto metadata_value = metadata.take_value();
    auto legacy_manifest = imageformat::inspect_backup_manifest_v1(
        metadata_value.backup_manifest);
    if (!legacy_manifest) {
      return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
          legacy_manifest.error());
    }
    hashes = make_tsumugi_source_identity_hashes(
        source.value().observed.identity,
        observed.physical_sector_size,
        metadata_value.partition_table_snapshot);
    if (!hashes) {
      return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
          hashes.error());
    }
    manifest = convert_mbr_manifest(
        request,
        source.value(),
        legacy_manifest.value(),
        metadata_value.layout,
        whole_selection.value(),
        metadata_value.partition_table_snapshot,
        hashes.value());
  }
  if (!manifest) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        manifest.error());
  }
  manifest.value().mode = imageformat::TsumugiManifestMode::rescue;
  const auto imaged_partition_count = static_cast<std::uint32_t>(
      manifest.value().partitions.size());
  auto sizing = rescue_image_sizing(manifest.value());
  if (!sizing) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        sizing.error());
  }
  std::uint64_t required_available_bytes{};
  if (!checked_add(
          source.value().observed.identity.size_bytes,
          sizing.value().maximum_image_bytes,
          required_available_bytes)) {
    return failure<WindowsDataRescueImageCreateReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Windowsデータ救出Tsumugi必要容量",
        L"RAW一時領域と最大画像寸法の合計が64bit上限を超えます");
  }
  destination_guard.required_available_bytes = required_available_bytes;
  destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        destination.error());
  }

  WindowsDataRescueSourceSession source_session(
      std::move(source.value().reader), hashes.value());
  auto staging = dependencies.make_rescue_staging(
      imageformat::WindowsTsumugiRescueStagingRequest{
          .final_path = request.final_path,
          .expected_source_disk = source.value().observed.identity,
          .source_disk_size = source_session.size_bytes(),
          .logical_sector_size = source_session.logical_sector_size(),
          .source_model_hash = hashes.value().model,
          .source_serial_hash = hashes.value().serial,
          .source_state_hash = hashes.value().locked_state,
          .required_available_bytes = required_available_bytes,
          .replace_existing = request.replace_existing,
      });
  if (!staging) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        staging.error());
  }

  clonecore::RescueCopyCallbacks rescue_callbacks{
      .cancellation_requested = request.callbacks.cancellation_requested,
      .safe_boundary = request.callbacks.safe_boundary,
  };
  if (request.callbacks.progress) {
    rescue_callbacks.progress =
        [&callbacks = request.callbacks](
            const clonecore::RescueCopyProgress& progress) {
          clonecore::report_disk_operation_progress(
              callbacks, translate_rescue_progress(progress));
        };
  }
  imageformat::TsumugiImageCreateRequest image_request{
      .final_path = request.final_path,
      .storage_file_system = storage_file_system.value(),
      .manifest = manifest.take_value(),
      .compression = imageformat::ImageCompression::zstandard,
      .chunk_size = imageformat::kImageChunkSize16MiB,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .verification_mode = request.verification_mode,
      .replace_existing = request.replace_existing,
  };
  if (request.encryption_password.has_value()) {
    image_request.encryption = imageformat::TsumugiImageEncryptionRequest{
        .password = *request.encryption_password,
    };
  }
  auto created = imageformat::create_tsumugi_rescue_image_v1(
      imageformat::TsumugiRescueImageCreateRequest{
          .image = std::move(image_request),
          .rescue_copy = clonecore::RescueRawCopyRequest{
              .environment = clonecore::RescueExecutionEnvironment::windows,
              .source_kind = clonecore::RescueSourceKind::data_disk,
              .rescue_mode_explicitly_confirmed = true,
              .large_block_bytes = 4U * 1024U * 1024U,
              .callbacks = std::move(rescue_callbacks),
          },
          .failing_source = &source_session,
          .staging = staging.value().get(),
      },
      request.callbacks);
  if (!created) {
    return clonecore::Result<WindowsDataRescueImageCreateReport>::failure(
        created.error());
  }
  if (!imageformat::selected_tsumugi_creation_verification_passed(
          created.value().image) ||
      !created.value().image.stream.committed ||
      !created.value().staging_sealed_for_image_read ||
      !created.value().staging_discarded_before_final_commit ||
      !created.value()
           .staging_destination_revalidated_before_final_commit) {
    return failure<WindowsDataRescueImageCreateReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Windowsデータ救出Tsumugi作成結果",
        L"救出一時領域の封印・破棄・保存先再識別、選択済み画像検証、または完成名確定を確認できません");
  }
  return clonecore::Result<WindowsDataRescueImageCreateReport>::success({
      .source_identity = source.value().observed.identity,
      .source_partition_style = observed.partition_style,
      .imaged_partition_count = imaged_partition_count,
      .logical_payload_bytes = sizing.value().logical_payload_bytes,
      .source_was_read_only_or_offline = true,
      .rescue = std::move(created.value()),
  });
}

clonecore::Result<WindowsDataRescueImageCreateReport>
execute_windows_data_rescue_image_create_with_windows_apis(
    const OnlineImageCreateRequest& request) {
  return execute_windows_data_rescue_image_create(
      request,
      WindowsDataRescueImageCreateDependencies{
          .open_read_only_disk =
              [](const clonecore::StableDiskIdentity& expected) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        expected);
              },
          .query_destination_file_system =
              query_destination_file_system_with_windows_apis,
          .validate_destination =
              imageformat::validate_windows_tsumugi_destination,
          .make_rescue_staging =
              [](const imageformat::WindowsTsumugiRescueStagingRequest&
                     staging) {
                return imageformat::
                    make_windows_tsumugi_rescue_staging_session(staging);
              },
      });
}

}  // namespace ytec::windowsapp

#include "ytec/windowsapp/online_shrink_image_plan.h"

#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace ytec::windowsapp {
namespace {

clonecore::Error plan_error(
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
  return clonecore::Result<T>::failure(plan_error(
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

clonecore::Result<std::string> utf16_to_utf8(
    const std::wstring_view value,
    const std::wstring_view operation) {
  if (value.empty()) {
    return clonecore::Result<std::string>::success({});
  }
  if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が長すぎます");
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
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        std::wstring(operation),
        L"UTF-16文字列をUTF-8へ変換できません");
  }
  std::string output(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          output.data(),
          length,
          nullptr,
          nullptr) != length) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        std::wstring(operation),
        L"UTF-8文字列を完全に生成できません");
  }
  return clonecore::Result<std::string>::success(std::move(output));
}

imageformat::TsumugiManifestPartitionRole convert_role(
    const migrationcore::MigrationPartitionRole role) noexcept {
  using Source = migrationcore::MigrationPartitionRole;
  using Target = imageformat::TsumugiManifestPartitionRole;
  switch (role) {
    case Source::efi_system:
      return Target::efi_system;
    case Source::microsoft_reserved:
      return Target::microsoft_reserved;
    case Source::bios_system:
      return Target::bios_system;
    case Source::windows:
      return Target::windows;
    case Source::recovery:
      return Target::recovery;
    case Source::data:
      return Target::data;
  }
  return Target::other;
}

imageformat::TsumugiManifestFileSystem convert_file_system(
    const migrationcore::MigrationFileSystem file_system) noexcept {
  using Source = migrationcore::MigrationFileSystem;
  using Target = imageformat::TsumugiManifestFileSystem;
  switch (file_system) {
    case Source::none:
      return Target::none;
    case Source::ntfs:
      return Target::ntfs;
    case Source::fat32:
      return Target::fat32;
    case Source::exfat:
      return Target::exfat;
    case Source::unsupported:
      return Target::unknown;
  }
  return Target::unknown;
}

bool system_required_role(
    const migrationcore::MigrationPartitionRole role) noexcept {
  using Role = migrationcore::MigrationPartitionRole;
  return role == Role::efi_system || role == Role::microsoft_reserved ||
      role == Role::bios_system || role == Role::windows ||
      role == Role::recovery;
}

bool supported_storage(
    const imageformat::TsumugiImageStorageFileSystem file_system) noexcept {
  return file_system == imageformat::TsumugiImageStorageFileSystem::ntfs ||
      file_system == imageformat::TsumugiImageStorageFileSystem::exfat;
}

bool valid_final_path(const std::wstring& value) {
  const std::filesystem::path path(value);
  return path.is_absolute() && !path.parent_path().empty() &&
      _wcsicmp(path.extension().c_str(), L".tsumugi") == 0 &&
      path.filename() != L".tsumugi";
}

}  // namespace

clonecore::Result<WindowsOnlineShrinkImagePlan>
plan_windows_online_shrink_image(
    const WindowsOnlineShrinkImagePlanRequest& request) {
  const auto& analysis = request.analysis;
  const auto identity = clonecore::validate_stable_identity(
      analysis.source, analysis.source, L"縮小Tsumugiコピー元");
  if (!identity) {
    return clonecore::Result<WindowsOnlineShrinkImagePlan>::failure(
        identity.error());
  }
  const bool source_contains_windows = analysis.source.is_system_disk;
  if (!request.administrator || !valid_final_path(request.final_path) ||
      !supported_storage(request.storage_file_system) ||
      !imageformat::is_supported_tsumugi_create_verification_mode(
          request.verification_mode) ||
      (request.chunk_size != imageformat::kImageChunkSize16MiB &&
       request.chunk_size != imageformat::kImageChunkSize32MiB) ||
      analysis.physical_sector_size == 0U ||
      analysis.physical_sector_size < analysis.source.logical_sector_size ||
      analysis.physical_sector_size % analysis.source.logical_sector_size !=
          0U ||
      analysis.created_utc.empty() || !analysis.created_utc.ends_with('Z') ||
      analysis.app_version.empty() || analysis.partitions.empty() ||
      !analysis.bitlocker_fully_decrypted ||
      source_contains_windows != analysis.windows_version.has_value()) {
    return failure<WindowsOnlineShrinkImagePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows縮小Tsumugi計画",
        L"管理者、保存先、セクター、Windows識別、BitLocker、または作成情報が対応条件外です");
  }

  const auto snapshot = imageformat::inspect_partition_snapshot_v1(
      analysis.partition_snapshot);
  const auto expected_style =
      analysis.partition_style == migrationcore::MigrationPartitionStyle::gpt
      ? imageformat::PartitionTableStyle::gpt
      : imageformat::PartitionTableStyle::mbr;
  if (!snapshot || snapshot.value().style != expected_style ||
      snapshot.value().source_disk_size != analysis.source.size_bytes ||
      snapshot.value().logical_sector_size !=
          analysis.source.logical_sector_size) {
    return failure<WindowsOnlineShrinkImagePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小Tsumugiパーティション表",
        L"読取り専用解析とPartitionSnapshotの形式または寸法が一致しません");
  }

  auto model_hash = imageformat::hash_tsumugi_source_model_v1(
      analysis.source.model);
  auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
      analysis.source.serial_suffix, analysis.source.device_instance_id);
  if (!model_hash || !serial_hash) {
    return clonecore::Result<WindowsOnlineShrinkImagePlan>::failure(
        model_hash ? serial_hash.error() : model_hash.error());
  }

  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::shrink,
      .partition_style = analysis.partition_style ==
              migrationcore::MigrationPartitionStyle::gpt
          ? imageformat::TsumugiManifestPartitionStyle::gpt
          : imageformat::TsumugiManifestPartitionStyle::mbr,
      .flags = imageformat::
          TsumugiManifestFlags::automatic_surplus_allocation,
      .source_disk_size = analysis.source.size_bytes,
      .logical_sector_size = analysis.source.logical_sector_size,
      .physical_sector_size = analysis.physical_sector_size,
      .source_model_hash = model_hash.take_value(),
      .source_serial_hash = serial_hash.take_value(),
      .created_utc = analysis.created_utc,
      .app_version = analysis.app_version,
      .partition_snapshot = analysis.partition_snapshot,
  };
  if (source_contains_windows) {
    manifest.flags = manifest.flags |
        imageformat::TsumugiManifestFlags::source_contains_windows;
  }

  std::map<std::uint32_t, std::wstring> content_volumes;
  for (const auto& volume : analysis.content_volumes) {
    if (volume.source_table_index == 0U ||
        volume.volume_guid_path.empty() ||
        !content_volumes
             .emplace(volume.source_table_index, volume.volume_guid_path)
             .second) {
      return failure<WindowsOnlineShrinkImagePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Windows縮小Tsumugi Volume対応",
          L"NTFSパーティションとVolume GUIDの対応が不正または重複しています");
    }
  }

  std::set<std::uint32_t> table_indexes;
  std::uint64_t previous_end{};
  std::size_t windows_count{};
  WindowsOnlineShrinkImagePlan result{};
  result.workflow.administrator = request.administrator;
  result.capture.source_disk = analysis.source;
  manifest.partitions.reserve(analysis.partitions.size());
  for (const auto& partition : analysis.partitions) {
    std::uint64_t end{};
    if (partition.source_table_index == 0U ||
        partition.source_size_bytes == 0U ||
        partition.source_offset_bytes % analysis.source.logical_sector_size !=
            0U ||
        partition.source_size_bytes % analysis.source.logical_sector_size !=
            0U ||
        !checked_add(
            partition.source_offset_bytes,
            partition.source_size_bytes,
            end) ||
        end > analysis.source.size_bytes ||
        partition.source_offset_bytes < previous_end ||
        partition.used_bytes > partition.source_size_bytes ||
        !table_indexes.insert(partition.source_table_index).second) {
      return failure<WindowsOnlineShrinkImagePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Windows縮小Tsumugi partition",
          L"パーティション番号、範囲、整列、順序、または使用量が不正です");
    }
    previous_end = end;

    const bool ntfs =
        partition.file_system == migrationcore::MigrationFileSystem::ntfs;
    const bool static_raw =
        partition.file_system == migrationcore::MigrationFileSystem::none ||
        partition.file_system ==
            migrationcore::MigrationFileSystem::unsupported;
    if (!ntfs && !static_raw) {
      return failure<WindowsOnlineShrinkImagePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小Tsumugi filesystem",
          L"現在のWindows直接縮小作成はNTFSと静的exact RAW領域だけに対応します");
    }
    if (ntfs &&
        (partition.cluster_size < analysis.source.logical_sector_size ||
         partition.cluster_size > 2ULL * 1024ULL * 1024ULL ||
         partition.cluster_size % analysis.source.logical_sector_size != 0U)) {
      return failure<WindowsOnlineShrinkImagePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小Tsumugi NTFSクラスター",
          L"NTFSクラスター寸法が対応範囲外です");
    }

    std::uint64_t minimum = partition.source_size_bytes;
    if (ntfs) {
      auto calculated = migrationcore::minimum_shrink_partition_bytes(
          migrationcore::ShrinkSourcePartition{
              .source_table_index = partition.source_table_index,
              .role = partition.role,
              .file_system = partition.file_system,
              .source_size_bytes = partition.source_size_bytes,
              .used_bytes = partition.used_bytes,
              .cluster_size = partition.cluster_size,
              .label = partition.label,
              .active = partition.active,
          });
      if (!calculated) {
        return clonecore::Result<WindowsOnlineShrinkImagePlan>::failure(
            calculated.error());
      }
      minimum = calculated.value();
      if (minimum > partition.source_size_bytes) {
        // A used source cannot be migrated to a target larger than itself and
        // still be described as a shrink-capable partition.
        minimum = partition.source_size_bytes;
      }
    }

    auto name = utf16_to_utf8(partition.name, L"縮小Tsumugi partition名");
    auto label = utf16_to_utf8(partition.label, L"縮小Tsumugi label");
    if (!name || !label) {
      return clonecore::Result<WindowsOnlineShrinkImagePlan>::failure(
          name ? label.error() : name.error());
    }

    auto flags = imageformat::TsumugiManifestPartitionFlags::selected;
    if (partition.active) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::active;
    }
    if (source_contains_windows && system_required_role(partition.role)) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::required;
    }
    if (partition.role == migrationcore::MigrationPartitionRole::windows) {
      flags = flags |
          imageformat::TsumugiManifestPartitionFlags::contains_windows;
      ++windows_count;
    }
    manifest.partitions.push_back(imageformat::TsumugiManifestPartition{
        .source_table_index = partition.source_table_index,
        .source_partition_number = partition.source_table_index,
        .role = convert_role(partition.role),
        .file_system = convert_file_system(partition.file_system),
        .flags = flags,
        .source_offset = partition.source_offset_bytes,
        .source_size = partition.source_size_bytes,
        .used_bytes = partition.used_bytes,
        .minimum_target_bytes = minimum,
        .planned_target_bytes = minimum,
        .cluster_size = ntfs ? partition.cluster_size : 0U,
        .type_id = partition.type_id,
        .unique_id = partition.unique_id,
        .name_utf8 = name.take_value(),
        .label_utf8 = label.take_value(),
    });

    if (ntfs) {
      const auto volume = content_volumes.find(partition.source_table_index);
      if (volume == content_volumes.end()) {
        return failure<WindowsOnlineShrinkImagePlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Windows縮小Tsumugi NTFS対応",
            L"選択NTFSに対応するVolume GUIDがありません");
      }
      result.workflow.volumes.push_back(vssrequester::VolumeRequest{
          .volume_guid_path = volume->second,
          .file_system = L"NTFS",
      });
      result.capture.snapshot_volumes.push_back(
          WindowsNtfsShrinkVolumeSelection{
              .source_table_index = partition.source_table_index,
              .original_volume_guid_path = volume->second,
          });
    }
  }
  if (windows_count != (source_contains_windows ? 1U : 0U) ||
      result.workflow.volumes.empty() ||
      result.workflow.volumes.size() != content_volumes.size()) {
    return failure<WindowsOnlineShrinkImagePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小Tsumugi構成",
        L"Windows領域またはNTFS Volume対応の件数が解析結果と一致しません");
  }

  result.image_template = imageformat::TsumugiImageCreateRequest{
      .final_path = request.final_path,
      .storage_file_system = request.storage_file_system,
      .manifest = manifest,
      .compression = imageformat::ImageCompression::zstandard,
      .chunk_size = request.chunk_size,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .verification_mode = request.verification_mode,
      .encryption = request.encryption_password.has_value()
          ? std::optional<imageformat::TsumugiImageEncryptionRequest>(
                imageformat::TsumugiImageEncryptionRequest{
                    .password = request.encryption_password.value(),
                })
          : std::nullopt,
      .replace_existing = request.replace_existing,
  };
  result.capture.reviewed_manifest = std::move(manifest);
  return clonecore::Result<WindowsOnlineShrinkImagePlan>::success(
      std::move(result));
}

}  // namespace ytec::windowsapp

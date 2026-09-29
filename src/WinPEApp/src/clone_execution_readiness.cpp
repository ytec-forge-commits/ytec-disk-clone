#include "ytec/winpeapp/clone_execution_readiness.h"

#include "ytec/diskmodel/clone_target_layout.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::winpeapp {
namespace {

constexpr std::wstring_view kGptEfiType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kGptMsrType =
    L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}";
constexpr std::wstring_view kGptBasicDataType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::wstring_view kGptRecoveryType =
    L"{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}";
constexpr std::wstring_view kGptLdmMetadataType =
    L"{5808C8AA-7E8F-42E0-85D2-E1E90434CFB3}";
constexpr std::wstring_view kGptLdmDataType =
    L"{AF9B60A0-1431-4F62-BC68-3311714A69AD}";
constexpr std::wstring_view kStorageSpacesProtectiveType =
    L"{E75CAF8F-F680-4CEE-AFA3-B001E56EFC2D}";

clonecore::Error readiness_error(
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
          [](const wchar_t lhs, const wchar_t rhs) {
            return std::towupper(lhs) == std::towupper(rhs);
          });
}

bool starts_with_case_insensitive(
    const std::wstring_view value,
    const std::wstring_view prefix) {
  return value.size() >= prefix.size() &&
      std::equal(
          prefix.begin(),
          prefix.end(),
          value.begin(),
          [](const wchar_t lhs, const wchar_t rhs) {
            return std::towupper(lhs) == std::towupper(rhs);
          });
}

bool bus_is_known_basic_storage(const diskmodel::DiskInfo& disk) {
  return !disk.bus_type.empty() &&
      !starts_with_case_insensitive(disk.bus_type, L"Unknown") &&
      !equals_case_insensitive(disk.bus_type, L"Storage Spaces");
}

bool is_supported_gpt_type(const std::wstring_view type) {
  return equals_case_insensitive(type, kGptEfiType) ||
      equals_case_insensitive(type, kGptMsrType) ||
      equals_case_insensitive(type, kGptBasicDataType) ||
      equals_case_insensitive(type, kGptRecoveryType);
}

bool is_supported_mbr_type(const std::wstring_view type) {
  return equals_case_insensitive(type, L"0x07") ||
      equals_case_insensitive(type, L"0x27") ||
      equals_case_insensitive(type, L"0x0B") ||
      equals_case_insensitive(type, L"0x0C");
}

bool has_explicitly_unsupported_partition(
    const diskmodel::DiskInfo& disk) {
  return std::any_of(
      disk.partitions.begin(),
      disk.partitions.end(),
      [](const diskmodel::PartitionInfo& partition) {
        return equals_case_insensitive(partition.type, L"0x42") ||
            equals_case_insensitive(partition.type, kGptLdmMetadataType) ||
            equals_case_insensitive(partition.type, kGptLdmDataType) ||
            equals_case_insensitive(
                partition.type, kStorageSpacesProtectiveType);
      });
}

bool all_partition_types_supported(const diskmodel::DiskInfo& disk) {
  return std::all_of(
      disk.partitions.begin(),
      disk.partitions.end(),
      [&](const diskmodel::PartitionInfo& partition) {
        if (disk.partition_style == diskmodel::PartitionStyle::gpt) {
          return is_supported_gpt_type(partition.type);
        }
        if (disk.partition_style == diskmodel::PartitionStyle::mbr) {
          return is_supported_mbr_type(partition.type);
        }
        return false;
      });
}

}  // namespace

clonecore::Status validate_clone_execution_observation(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    const bool require_target_same_or_larger) {
  if (source.is_system_disk || target.is_system_disk) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"WinPEクローンのシステムディスク保護",
        L"実行中システムの物理ディスクはコピー元・コピー先にできません"));
  }
  if (!source.offline.has_value() || !source.read_only.has_value() ||
      !source.removable.has_value() || !target.offline.has_value() ||
      !target.read_only.has_value() || !target.removable.has_value()) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"WinPEクローンのディスク属性",
        L"コピー元・コピー先のoffline、read-only、removable属性を確定できません"));
  }
  if (source.offline.value() || source.removable.value()) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_READY,
        L"WinPEクローンのコピー元属性",
        L"オンラインの固定ディスクだけをコピー元にできます"));
  }
  if (target.read_only.value() || target.removable.value()) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        target.read_only.value() ? ERROR_WRITE_PROTECT : ERROR_ACCESS_DENIED,
        L"WinPEクローンのコピー先属性",
        L"書込み可能な固定ディスクだけをコピー先にできます"));
  }
  if (diskmodel::disk_health_operation_advice(target.health, false) ==
      diskmodel::DiskHealthOperationAdvice::block_target) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_DEVICE_HARDWARE_ERROR,
        L"WinPEクローンのコピー先健康状態",
        L"SMARTまたはNVMeが注意・異常を報告しているディスクはコピー先にできません"));
  }
  if (!bus_is_known_basic_storage(source) ||
      !bus_is_known_basic_storage(target)) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPEクローンのBusType",
        L"BusType不明またはStorage Spacesのディスクは対象にできません"));
  }
  if (source.logical_sector_size != 512 ||
      target.logical_sector_size != 512 ||
      source.logical_sector_size != target.logical_sector_size) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPEクローンの論理セクター",
        L"実機相当検証までは同じ512バイト論理セクターだけを対象にします"));
  }
  if (source.size_bytes == 0 || target.size_bytes == 0U ||
      (require_target_same_or_larger &&
       target.size_bytes < source.size_bytes)) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"WinPEクローンのコピー先容量",
        require_target_same_or_larger
            ? L"通常モードのコピー先はコピー元と同じ容量以上が必要です"
            : L"コピー元またはコピー先の容量が不正です"));
  }
  if (source.partitions.empty() ||
      (source.partition_style != diskmodel::PartitionStyle::gpt &&
       source.partition_style != diskmodel::PartitionStyle::mbr)) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPEクローンのコピー元構成",
        L"パーティションを持つ基本GPTまたはMBRディスクが必要です"));
  }
  if (diskmodel::classify_clone_target_layout(target) ==
      diskmodel::CloneTargetLayoutKind::unsupported) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"WinPEクローンのコピー先初期状態",
        L"空のRAWまたは既知の基本GPT/MBRディスクだけをコピー先にできます"));
  }
  if (has_explicitly_unsupported_partition(source) ||
      !all_partition_types_supported(source)) {
    return clonecore::Status::failure(readiness_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPEクローンのパーティション形式",
        L"未対応、LDM、Storage Spacesのパーティションを含むため停止しました"));
  }
  return clonecore::success_status();
}

}  // namespace ytec::winpeapp

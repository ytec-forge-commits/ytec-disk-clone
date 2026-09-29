#include "ytec/diskmodel/clone_target_layout.h"

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace ytec::diskmodel {
namespace {

constexpr std::wstring_view kGptEfiType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kGptMsrType =
    L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}";
constexpr std::wstring_view kGptBasicDataType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::wstring_view kGptRecoveryType =
    L"{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}";

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t lhs, const wchar_t rhs) {
            return std::towupper(lhs) == std::towupper(rhs);
          });
}

bool is_supported_gpt_type(const std::wstring_view type) noexcept {
  return equals_case_insensitive(type, kGptEfiType) ||
      equals_case_insensitive(type, kGptMsrType) ||
      equals_case_insensitive(type, kGptBasicDataType) ||
      equals_case_insensitive(type, kGptRecoveryType);
}

bool is_supported_mbr_type(const std::wstring_view type) noexcept {
  return equals_case_insensitive(type, L"0x07") ||
      equals_case_insensitive(type, L"0x27") ||
      equals_case_insensitive(type, L"0x0B") ||
      equals_case_insensitive(type, L"0x0C");
}

}  // namespace

CloneTargetLayoutKind classify_clone_target_layout(
    const DiskInfo& disk) noexcept {
  if (disk.partition_style == PartitionStyle::raw) {
    return disk.partitions.empty()
        ? CloneTargetLayoutKind::empty_raw
        : CloneTargetLayoutKind::unsupported;
  }
  if (disk.partition_style != PartitionStyle::gpt &&
      disk.partition_style != PartitionStyle::mbr) {
    return CloneTargetLayoutKind::unsupported;
  }

  const bool supported = std::all_of(
      disk.partitions.begin(),
      disk.partitions.end(),
      [&](const PartitionInfo& partition) {
        if (partition.style != disk.partition_style) {
          return false;
        }
        return disk.partition_style == PartitionStyle::gpt
            ? is_supported_gpt_type(partition.type)
            : is_supported_mbr_type(partition.type);
      });
  return supported
      ? CloneTargetLayoutKind::supported_initialized
      : CloneTargetLayoutKind::unsupported;
}

}  // namespace ytec::diskmodel

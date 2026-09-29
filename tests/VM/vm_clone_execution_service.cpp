#include "vm_clone_execution_service.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/clonecore/offline_mbr_clone.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This service must never be built as a product target.
#endif

namespace ytec::vmtest {
namespace {

constexpr std::wstring_view kVmAuthorization =
    L"YTEC-VM-ONLY-PHASE1-DESTRUCTIVE";
constexpr std::wstring_view kVmBootAuthorization =
    L"YTEC-VM-ONLY-PHASE1-BOOT-CLONE";
constexpr std::wstring_view kVmLegacyBiosAuthorization =
    L"YTEC-VM-ONLY-PHASE3-MBR-CLONE";
constexpr std::wstring_view kVmLegacyBiosX64Authorization =
    L"YTEC-VM-ONLY-PRODUCT-MBR-X64-CLONE";
constexpr std::uint64_t kMinimumTestDiskBytes = 64U * 1024U * 1024U;
constexpr std::uint64_t kMaximumTestDiskBytes =
    8ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kBootSourceDiskBytes =
    96ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kBootTargetDiskBytes =
    110ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kLegacyBiosSourceDiskBytes =
    48ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kLegacyBiosTargetDiskBytes =
    56ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kLegacyBiosX64SourceDiskBytes =
    56ULL * 1024U * 1024U * 1024U;
constexpr std::uint64_t kLegacyBiosX64TargetDiskBytes =
    64ULL * 1024U * 1024U * 1024U;

clonecore::Error vm_error(
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

std::optional<std::wstring> registry_string(
    const wchar_t* const subkey,
    const wchar_t* const value_name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegGetValueW(
          HKEY_LOCAL_MACHINE,
          subkey,
          value_name,
          RRF_RT_REG_SZ,
          &type,
          nullptr,
          &bytes) != ERROR_SUCCESS ||
      bytes < sizeof(wchar_t) || bytes > 64U * 1024U) {
    return std::nullopt;
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(
          HKEY_LOCAL_MACHINE,
          subkey,
          value_name,
          RRF_RT_REG_SZ,
          &type,
          value.data(),
          &bytes) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

bool contains_virtualbox(const std::optional<std::wstring>& value) {
  if (!value.has_value()) {
    return false;
  }
  std::wstring normalized = value.value();
  std::transform(
      normalized.begin(),
      normalized.end(),
      normalized.begin(),
      [](const wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
      });
  return normalized.find(L"VIRTUALBOX") != std::wstring::npos ||
         normalized.find(L"INNOTEK") != std::wstring::npos;
}

bool contains_ascii(
    const std::vector<std::uint8_t>& bytes,
    const std::string_view needle) {
  if (bytes.size() < needle.size()) {
    return false;
  }
  for (std::size_t offset = 0;
       offset <= bytes.size() - needle.size();
       ++offset) {
    bool matches = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      unsigned char character = bytes[offset + index];
      if (character >= 'a' && character <= 'z') {
        character = static_cast<unsigned char>(character - 'a' + 'A');
      }
      if (character != static_cast<unsigned char>(needle[index])) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

bool is_virtualbox_disk(const diskmodel::DiskInfo& disk) {
  std::wstring model = disk.model;
  std::transform(
      model.begin(), model.end(), model.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
      });
  return model.find(L"VBOX") != std::wstring::npos &&
         disk.logical_sector_size == 512 && !disk.is_system_disk &&
         disk.serial_suffix.size() >= 4;
}

bool matches_profile(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    const VmCloneProfile profile) {
  if (!is_virtualbox_disk(source) || !is_virtualbox_disk(target)) {
    return false;
  }
  if (profile == VmCloneProfile::boot) {
    return source.size_bytes == kBootSourceDiskBytes &&
           target.size_bytes == kBootTargetDiskBytes;
  }
  if (profile == VmCloneProfile::legacy_bios) {
    return source.size_bytes == kLegacyBiosSourceDiskBytes &&
           target.size_bytes == kLegacyBiosTargetDiskBytes;
  }
  if (profile == VmCloneProfile::legacy_bios_x64) {
    return source.size_bytes == kLegacyBiosX64SourceDiskBytes &&
           target.size_bytes == kLegacyBiosX64TargetDiskBytes;
  }
  return source.size_bytes >= kMinimumTestDiskBytes &&
         source.size_bytes <= kMaximumTestDiskBytes &&
         target.size_bytes >= kMinimumTestDiskBytes &&
         target.size_bytes <= kMaximumTestDiskBytes;
}

clonecore::Result<VmCloneProfile> profile_from_request(
    const winpeapp::CloneExecutionRequest& request) {
  if (request.expected_source.size_bytes == kLegacyBiosX64SourceDiskBytes &&
      request.expected_target.size_bytes == kLegacyBiosX64TargetDiskBytes) {
    return clonecore::Result<VmCloneProfile>::success(
        VmCloneProfile::legacy_bios_x64);
  }
  if (request.expected_source.size_bytes == kLegacyBiosSourceDiskBytes &&
      request.expected_target.size_bytes == kLegacyBiosTargetDiskBytes) {
    return clonecore::Result<VmCloneProfile>::success(
        VmCloneProfile::legacy_bios);
  }
  if (request.expected_source.size_bytes == kBootSourceDiskBytes &&
      request.expected_target.size_bytes == kBootTargetDiskBytes) {
    return clonecore::Result<VmCloneProfile>::success(VmCloneProfile::boot);
  }
  if (request.expected_source.size_bytes >= kMinimumTestDiskBytes &&
      request.expected_source.size_bytes <= kMaximumTestDiskBytes &&
      request.expected_target.size_bytes >= kMinimumTestDiskBytes &&
      request.expected_target.size_bytes <= kMaximumTestDiskBytes) {
    return clonecore::Result<VmCloneProfile>::success(
        VmCloneProfile::synthetic);
  }
  return clonecore::Result<VmCloneProfile>::failure(vm_error(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_ACCESS_DENIED,
      L"VM専用クローン容量プロファイル",
      L"8GiB以下の合成ディスク、固定96GiB→110GiB GPT、固定48GiB→56GiB MBR、または固定56GiB→64GiB x64 MBRだけを使用できます"));
}

clonecore::Result<winpeapp::CloneExecutionReport> run_gpt_clone(
    const VmCloneSelection& selection,
    const clonecore::TargetConfirmation& confirmation,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto handles = diskmodel::open_verified_physical_clone_with_windows_apis(
      selection.source_identity,
      selection.target_identity,
      confirmation);
  if (!handles) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        handles.error());
  }
  const auto source_gpt = clonecore::parse_gpt(*handles.value().source);
  if (!source_gpt) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        source_gpt.error());
  }
  auto bindings = diskmodel::query_windows_volume_bitmap_bindings(
      handles.value().observed.source, source_gpt.value());
  if (!bindings) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        bindings.error());
  }
  clonecore::WindowsVolumeBitmapProvider bitmap_provider(
      bindings.take_value());
  auto guid_generator = clonecore::make_windows_guid_generator();
  const auto report = clonecore::execute_offline_gpt_clone(
      clonecore::OfflineGptCloneRequest{
          .expected_source = selection.source_identity,
          .observed_source = handles.value().observed.source_identity,
          .expected_target = selection.target_identity,
          .observed_target = handles.value().observed.target_identity,
          .confirmation = confirmation,
          .maximum_chunk_bytes = 1024U * 1024U,
          .callbacks = callbacks,
      },
      *handles.value().source,
      *handles.value().target,
      bitmap_provider,
      *guid_generator);
  if (!report) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        report.error());
  }
  return clonecore::Result<winpeapp::CloneExecutionReport>::success(
      winpeapp::CloneExecutionReport{
          .partition_style = winpeapp::ClonePartitionStyle::gpt,
          .copied_data_bytes = report.value().copied_data_bytes,
          .copied_partition_count = report.value().copied_partition_count,
          .recreated_partition_count =
              report.value().recreated_partition_count,
          .read_back_verified = report.value().read_back_verified,
          .partition_table_committed = report.value().primary_gpt_committed,
      });
}

clonecore::Result<winpeapp::CloneExecutionReport> run_mbr_clone(
    const VmCloneSelection& selection,
    const clonecore::TargetConfirmation& confirmation,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto handles = diskmodel::open_verified_physical_clone_with_windows_apis(
      selection.source_identity,
      selection.target_identity,
      confirmation);
  if (!handles) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        handles.error());
  }
  const auto source_mbr = clonecore::parse_mbr(*handles.value().source);
  if (!source_mbr) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        source_mbr.error());
  }
  auto bindings = diskmodel::query_windows_volume_bitmap_bindings(
      handles.value().observed.source, source_mbr.value());
  if (!bindings) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        bindings.error());
  }
  clonecore::WindowsVolumeBitmapProvider bitmap_provider(
      bindings.take_value());
  auto signature_generator = clonecore::make_windows_mbr_signature_generator();
  const auto report = clonecore::execute_offline_mbr_clone(
      clonecore::OfflineMbrCloneRequest{
          .expected_source = selection.source_identity,
          .observed_source = handles.value().observed.source_identity,
          .expected_target = selection.target_identity,
          .observed_target = handles.value().observed.target_identity,
          .confirmation = confirmation,
          .maximum_chunk_bytes = 1024U * 1024U,
          .connected_mbr_signatures = {source_mbr.value().disk_signature},
          .callbacks = callbacks,
      },
      *handles.value().source,
      *handles.value().target,
      bitmap_provider,
      *signature_generator);
  if (!report) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        report.error());
  }
  return clonecore::Result<winpeapp::CloneExecutionReport>::success(
      winpeapp::CloneExecutionReport{
          .partition_style = winpeapp::ClonePartitionStyle::mbr,
          .copied_data_bytes = report.value().copied_data_bytes,
          .copied_partition_count = report.value().copied_partition_count,
          .recreated_partition_count = 0,
          .read_back_verified = report.value().read_back_verified,
          .partition_table_committed = report.value().target_mbr_committed,
      });
}

}  // namespace

std::wstring_view vm_clone_authorization(const VmCloneProfile profile) {
  switch (profile) {
    case VmCloneProfile::boot:
      return kVmBootAuthorization;
    case VmCloneProfile::legacy_bios:
      return kVmLegacyBiosAuthorization;
    case VmCloneProfile::legacy_bios_x64:
      return kVmLegacyBiosX64Authorization;
    case VmCloneProfile::synthetic:
      return kVmAuthorization;
  }
  return {};
}

bool is_virtualbox_guest() {
  constexpr wchar_t kBiosKey[] = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
  if (contains_virtualbox(registry_string(kBiosKey, L"SystemProductName")) ||
      contains_virtualbox(registry_string(kBiosKey, L"SystemManufacturer"))) {
    return true;
  }

  constexpr DWORD kRawSmbiosProvider = 'RSMB';
  const UINT required_bytes =
      GetSystemFirmwareTable(kRawSmbiosProvider, 0, nullptr, 0);
  if (required_bytes == 0 || required_bytes > 1024U * 1024U) {
    return false;
  }
  std::vector<std::uint8_t> firmware(required_bytes);
  if (GetSystemFirmwareTable(
          kRawSmbiosProvider,
          0,
          firmware.data(),
          static_cast<DWORD>(firmware.size())) != required_bytes) {
    return false;
  }
  return contains_ascii(firmware, "VIRTUALBOX") ||
         contains_ascii(firmware, "INNOTEK");
}

bool is_administrator() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(
          &authority,
          2,
          SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS,
          0,
          0,
          0,
          0,
          0,
          0,
          &administrators)) {
    return false;
  }
  BOOL member = FALSE;
  const BOOL checked = CheckTokenMembership(nullptr, administrators, &member);
  FreeSid(administrators);
  return checked && member;
}

clonecore::Result<VmCloneSelection> select_vm_clone_disks(
    const std::uint32_t source_number,
    const std::uint32_t target_number,
    const VmCloneProfile profile) {
  auto provider = diskmodel::make_windows_disk_inventory_provider();
  const auto report = provider->enumerate();
  if (!report) {
    return clonecore::Result<VmCloneSelection>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<VmCloneSelection>::failure(vm_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"VM専用ハーネスの全ディスク列挙",
        L"未解決の列挙診断があるため実行できません"));
  }
  const auto source = std::find_if(
      report.value().disks.begin(),
      report.value().disks.end(),
      [&](const auto& disk) { return disk.disk_number == source_number; });
  const auto target = std::find_if(
      report.value().disks.begin(),
      report.value().disks.end(),
      [&](const auto& disk) { return disk.disk_number == target_number; });
  const bool found = source != report.value().disks.end() &&
                     target != report.value().disks.end();
  const diskmodel::PartitionStyle expected_source_style =
      profile == VmCloneProfile::legacy_bios ||
              profile == VmCloneProfile::legacy_bios_x64
          ? diskmodel::PartitionStyle::mbr
          : diskmodel::PartitionStyle::gpt;
  const bool eligible =
      found && matches_profile(*source, *target, profile) &&
      source->partition_style == expected_source_style &&
      target->partition_style == diskmodel::PartitionStyle::raw &&
      target->partitions.empty() && target->size_bytes >= source->size_bytes &&
      target->offline.has_value() && target->read_only.has_value() &&
      target->removable.has_value() && !target->read_only.value() &&
      !target->removable.value();
  if (!eligible) {
    return clonecore::Result<VmCloneSelection>::failure(vm_error(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_ACCESS_DENIED,
      L"VM専用合成ディスク制限",
      profile == VmCloneProfile::boot
          ? L"固定96GiBのVirtualBox GPTコピー元と固定110GiBのRAWコピー先だけを使用できます"
          : profile == VmCloneProfile::legacy_bios
              ? L"固定48GiBのVirtualBox MBRコピー元と固定56GiBのRAWコピー先だけを使用できます"
              : profile == VmCloneProfile::legacy_bios_x64
                  ? L"固定56GiBのVirtualBox x64 MBRコピー元と固定64GiBのRAWコピー先だけを使用できます"
              : L"8GiB以下のVirtualBox GPTコピー元と、それ以上の空RAWコピー先だけを使用できます"));
  }

  auto source_identity =
      diskmodel::make_stable_disk_identity(*source, source->is_system_disk);
  if (!source_identity) {
    return clonecore::Result<VmCloneSelection>::failure(
        source_identity.error());
  }
  auto target_identity =
      diskmodel::make_stable_disk_identity(*target, target->is_system_disk);
  if (!target_identity) {
    return clonecore::Result<VmCloneSelection>::failure(
        target_identity.error());
  }
  return clonecore::Result<VmCloneSelection>::success(VmCloneSelection{
      .source_disk = *source,
      .target_disk = *target,
      .source_identity = source_identity.take_value(),
      .target_identity = target_identity.take_value(),
  });
}

clonecore::Result<VmCloneSelection> select_unique_vm_clone_target(
    const std::uint32_t source_number,
    const VmCloneProfile profile) {
  auto provider = diskmodel::make_windows_disk_inventory_provider();
  const auto report = provider->enumerate();
  if (!report) {
    return clonecore::Result<VmCloneSelection>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<VmCloneSelection>::failure(vm_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"VM専用コピー先の全ディスク列挙",
        L"未解決の列挙診断があるため実行できません"));
  }

  std::optional<VmCloneSelection> selected;
  for (const auto& disk : report.value().disks) {
    if (disk.disk_number == source_number) {
      continue;
    }
    auto candidate = select_vm_clone_disks(
        source_number, disk.disk_number, profile);
    if (!candidate) {
      continue;
    }
    if (selected.has_value()) {
      return clonecore::Result<VmCloneSelection>::failure(vm_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_ACCESS_DENIED,
          L"VM専用合成コピー先の一意選択",
          L"条件を満たすRAWコピー先が複数あるため実行できません"));
    }
    selected = candidate.take_value();
  }
  if (!selected.has_value()) {
    return clonecore::Result<VmCloneSelection>::failure(vm_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"VM専用合成コピー先の一意選択",
        L"条件を満たすRAWコピー先がありません"));
  }
  return clonecore::Result<VmCloneSelection>::success(
      std::move(selected.value()));
}

clonecore::Result<winpeapp::CloneExecutionReport>
VmCloneExecutionService::execute(
    const winpeapp::CloneExecutionRequest& request) {
  const auto profile = profile_from_request(request);
  if (!profile) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        profile.error());
  }
  if (!is_virtualbox_guest()) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(vm_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"VM専用実行環境",
        L"VirtualBoxゲスト以外では実行できません"));
  }
  if (!is_administrator()) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(vm_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"VM専用管理者権限",
        L"管理者トークンが必要です"));
  }
  if (request.authorization != vm_clone_authorization(profile.value())) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(vm_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ACCESS_DENIED,
        L"VM専用許可語",
        L"固定許可語が一致しません"));
  }

  const auto selection = select_vm_clone_disks(
      request.expected_source.disk_number,
      request.expected_target.disk_number,
      profile.value());
  if (!selection) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        selection.error());
  }
  const auto identity_status = clonecore::validate_clone_identities(
      request.expected_source,
      selection.value().source_identity,
      request.expected_target,
      selection.value().target_identity,
      request.confirmation);
  if (!identity_status) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        identity_status.error());
  }

  const auto offline =
      diskmodel::set_verified_target_offline_with_windows_apis(
          selection.value().source_identity,
          selection.value().target_identity,
          request.confirmation,
          true);
  if (!offline) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        offline.error());
  }
  auto clone_report =
      profile.value() == VmCloneProfile::legacy_bios ||
              profile.value() == VmCloneProfile::legacy_bios_x64
      ? run_mbr_clone(
            selection.value(), request.confirmation, request.callbacks)
      : run_gpt_clone(
            selection.value(), request.confirmation, request.callbacks);
  if (!clone_report) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        clone_report.error());
  }
  const auto online =
      diskmodel::set_verified_target_offline_with_windows_apis(
          selection.value().source_identity,
          selection.value().target_identity,
          request.confirmation,
          false);
  if (!online) {
    return clonecore::Result<winpeapp::CloneExecutionReport>::failure(
        online.error());
  }

  auto final_report = clone_report.take_value();
  final_report.target_returned_online = true;
  return clonecore::Result<winpeapp::CloneExecutionReport>::success(
      std::move(final_report));
}

}  // namespace ytec::vmtest

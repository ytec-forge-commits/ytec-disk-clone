#include "vm_clone_execution_service.h"

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PRODUCT-MBR2GPT-VERIFIER";
constexpr std::uint64_t kSourceBytes =
    56ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetBytes =
    64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMarkerBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::string_view kMarker =
    "YDC_PRODUCT_MBR2GPT_PASS_V1";
constexpr std::string_view kProbeStagedMarker =
    "YDC_TARGET_PROBE_STAGED_V1";
constexpr std::wstring_view kEspType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kMsrType =
    L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}";
constexpr std::wstring_view kBasicDataType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";

bool equals_ignore_case(
    const std::wstring& left,
    const std::wstring_view right) {
  return _wcsicmp(left.c_str(), std::wstring(right).c_str()) == 0;
}

bool is_fixed_virtualbox_disk(const ytec::diskmodel::DiskInfo& disk) {
  return _wcsicmp(disk.model.c_str(), L"VBOX HARDDISK") == 0 &&
         disk.logical_sector_size == 512U &&
         disk.physical_sector_size >= 512U &&
         !disk.is_system_disk &&
         disk.offline.has_value() && !disk.offline.value() &&
         disk.read_only.has_value() && !disk.read_only.value() &&
         disk.removable.has_value() && !disk.removable.value() &&
         disk.serial_suffix.size() >= 4U;
}

struct VerifiedDisks final {
  ytec::diskmodel::DiskInfo source;
  ytec::diskmodel::DiskInfo target;
  ytec::diskmodel::DiskInfo marker;
};

constexpr std::string_view kTargetProbeScript = R"YDC($ErrorActionPreference='Stop'
$diskStyle='ERROR'
$isBoot=0
$isSystem=0
$partitionCount=0
$espCount=0
$msrCount=0
$secureBoot=0
$architecture64=0
$probeError=0
$shutdownScheduled=0
try {
    $disk=Get-Disk -Number 0
    $parts=@(Get-Partition -DiskNumber 0)
    $esp=@($parts | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' })
    $msr=@($parts | Where-Object { $_.GptType -eq '{e3c9e316-0b5c-4db8-817d-f92df00215ae}' })
    $diskStyle=[string]$disk.PartitionStyle
    $isBoot=[int][bool]$disk.IsBoot
    $isSystem=[int][bool]$disk.IsSystem
    $partitionCount=$parts.Count
    $espCount=$esp.Count
    $msrCount=$msr.Count
    $secureBoot=[int](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled).UEFISecureBootEnabled
    $architecture64=[int]([string](Get-CimInstance Win32_OperatingSystem).OSArchitecture -match '64')
}
catch {
    $probeError=1
}
$passed=(
    $probeError -eq 0 -and
    $diskStyle -eq 'GPT' -and
    $isBoot -eq 1 -and
    $isSystem -eq 1 -and
    $espCount -eq 1 -and
    $msrCount -eq 1 -and
    $secureBoot -eq 1 -and
    $architecture64 -eq 1
)
$marker=if($passed){'YDC_TARGET_SECURE_BOOT_PASS_V1'}else{'YDC_TARGET_SECURE_BOOT_FAIL_V1'}
& "$env:SystemRoot\System32\shutdown.exe" /s /t 5
$shutdownScheduled=1
$fields=@(
    $marker,
    ('DiskStyle='+$diskStyle),
    ('IsBoot='+$isBoot),
    ('IsSystem='+$isSystem),
    ('PartitionCount='+$partitionCount),
    ('EspCount='+$espCount),
    ('MsrCount='+$msrCount),
    ('SecureBoot='+$secureBoot),
    ('Architecture64='+$architecture64),
    ('ProbeError='+$probeError),
    ('ShutdownScheduled='+$shutdownScheduled)
)
$payload='YDCUART='+($fields -join ';')+';END'
& $env:ComSpec /d /c ('echo '+$payload+' >COM1') | Out-Null
)YDC";

constexpr std::string_view kTargetProbeLauncher =
    "@echo off\r\n"
    "powershell.exe -NoLogo -NoProfile -NonInteractive "
    "-WindowStyle Hidden -ExecutionPolicy Bypass -File "
    "\"%ProgramData%\\YdcMbr2GptValidation.ps1\"\r\n"
    "del /f /q \"%ProgramData%\\YdcMbr2GptValidation.ps1\"\r\n"
    "del /f /q \"%~f0\"\r\n";

bool write_new_regular_file(
    const std::wstring& path,
    const std::string_view contents) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    return false;
  }
  DWORD transferred = 0;
  return contents.size() <= MAXDWORD &&
         WriteFile(
             file.get(),
             contents.data(),
             static_cast<DWORD>(contents.size()),
             &transferred,
             nullptr) &&
         transferred == contents.size() && FlushFileBuffers(file.get());
}

bool is_regular_non_reparse_file(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool stage_target_startup_probe(
    const ytec::diskmodel::DiskInfo& target) {
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      target, false);
  if (!identity) {
    return false;
  }
  auto opened =
      ytec::diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          identity.value());
  if (!opened) {
    return false;
  }
  const auto gpt = ytec::clonecore::parse_gpt(*opened.value().reader);
  if (!gpt) {
    return false;
  }
  const auto bindings =
      ytec::diskmodel::query_windows_volume_bitmap_bindings(
          opened.value().observed.observed, gpt.value());
  if (!bindings) {
    return false;
  }

  std::wstring windows_root;
  for (const auto& binding : bindings.value()) {
    std::wstring root = binding.volume_device_path;
    if (root.empty() || root.back() != L'\\') {
      root.push_back(L'\\');
    }
    const std::wstring kernel =
        root + L"Windows\\System32\\ntoskrnl.exe";
    if (!is_regular_non_reparse_file(kernel)) {
      continue;
    }
    if (!windows_root.empty()) {
      return false;
    }
    windows_root = std::move(root);
  }
  if (windows_root.empty()) {
    return false;
  }

  const std::wstring program_data = windows_root + L"ProgramData\\";
  const std::wstring startup = program_data +
      L"Microsoft\\Windows\\Start Menu\\Programs\\Startup\\";
  const DWORD program_data_attributes =
      GetFileAttributesW(program_data.c_str());
  const DWORD startup_attributes = GetFileAttributesW(startup.c_str());
  if (program_data_attributes == INVALID_FILE_ATTRIBUTES ||
      (program_data_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (program_data_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      startup_attributes == INVALID_FILE_ATTRIBUTES ||
      (startup_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (startup_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;
  }
  const std::wstring script =
      program_data + L"YdcMbr2GptValidation.ps1";
  const std::wstring launcher = startup + L"YdcMbr2GptValidation.cmd";
  if (!write_new_regular_file(script, kTargetProbeScript)) {
    return false;
  }
  if (!write_new_regular_file(launcher, kTargetProbeLauncher)) {
    DeleteFileW(script.c_str());
    return false;
  }
  return is_regular_non_reparse_file(script) &&
         is_regular_non_reparse_file(launcher);
}

ytec::clonecore::Result<VerifiedDisks> verify_layout() {
  auto inventory = ytec::diskmodel::make_windows_disk_inventory_provider();
  auto report = inventory->enumerate();
  if (!report) {
    return ytec::clonecore::Result<VerifiedDisks>::failure(report.error());
  }
  if (!report.value().issues.empty() || report.value().disks.size() != 3U) {
    return ytec::clonecore::Result<VerifiedDisks>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::verification_failed,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"VM MBR2GPT verifier disk inventory",
            .message = L"Expected exactly three issue-free VirtualBox disks",
        });
  }

  const ytec::diskmodel::DiskInfo* source = nullptr;
  const ytec::diskmodel::DiskInfo* target = nullptr;
  const ytec::diskmodel::DiskInfo* marker = nullptr;
  for (const auto& disk : report.value().disks) {
    if (!is_fixed_virtualbox_disk(disk)) {
      return ytec::clonecore::Result<VerifiedDisks>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::unsupported_layout,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"VM MBR2GPT verifier disk identity",
              .message = L"A disk is not the fixed VirtualBox test shape",
          });
    }
    if (disk.size_bytes == kSourceBytes) {
      source = &disk;
    } else if (disk.size_bytes == kTargetBytes) {
      target = &disk;
    } else if (disk.size_bytes == kMarkerBytes) {
      marker = &disk;
    } else {
      return ytec::clonecore::Result<VerifiedDisks>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::unsupported_layout,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"VM MBR2GPT verifier disk capacity",
              .message = L"An unexpected disk capacity was observed",
          });
    }
  }
  if (source == nullptr || target == nullptr || marker == nullptr ||
      source->disk_number != 0U ||
      source->partition_style != ytec::diskmodel::PartitionStyle::mbr ||
      source->partitions.empty() ||
      target->partition_style != ytec::diskmodel::PartitionStyle::gpt ||
      target->partitions.empty() ||
      marker->partition_style != ytec::diskmodel::PartitionStyle::raw ||
      !marker->partitions.empty()) {
    return ytec::clonecore::Result<VerifiedDisks>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::verification_failed,
            .native_code = ERROR_CRC,
            .operation = L"VM MBR2GPT verifier partition styles",
            .message = L"Source MBR, target GPT, or marker RAW state is invalid",
        });
  }

  const auto count_type = [&](const std::wstring_view type) {
    return static_cast<std::size_t>(std::count_if(
        target->partitions.begin(),
        target->partitions.end(),
        [&](const ytec::diskmodel::PartitionInfo& partition) {
          return partition.style == ytec::diskmodel::PartitionStyle::gpt &&
                 equals_ignore_case(partition.type, type);
        }));
  };
  if (count_type(kEspType) != 1U || count_type(kMsrType) != 1U ||
      count_type(kBasicDataType) < 1U) {
    return ytec::clonecore::Result<VerifiedDisks>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::verification_failed,
            .native_code = ERROR_CRC,
            .operation = L"VM MBR2GPT verifier GPT roles",
            .message = L"Exactly one ESP/MSR and at least one Basic Data partition are required",
        });
  }
  return ytec::clonecore::Result<VerifiedDisks>::success(
      VerifiedDisks{*source, *target, *marker});
}

int write_verified_marker() {
  const auto verified = verify_layout();
  if (!verified) {
    std::wcerr << verified.error().operation << L": "
               << verified.error().message << L"\n";
    return 4;
  }
  if (!stage_target_startup_probe(verified.value().target)) {
    std::wcerr << L"Unable to stage the fixed target startup probe.\n";
    return 13;
  }
  const auto expected = ytec::diskmodel::make_stable_disk_identity(
      verified.value().marker, false);
  if (!expected) {
    std::wcerr << expected.error().operation << L": "
               << expected.error().message << L"\n";
    return 5;
  }

  const std::wstring path = L"\\\\.\\PhysicalDrive" +
      std::to_wstring(verified.value().marker.disk_number);
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!handle) {
    std::wcerr << L"Unable to open the dedicated marker disk.\n";
    return 6;
  }

  const auto fresh = verify_layout();
  if (!fresh) {
    std::wcerr << fresh.error().operation << L": "
               << fresh.error().message << L"\n";
    return 7;
  }
  const auto observed = ytec::diskmodel::make_stable_disk_identity(
      fresh.value().marker, false);
  if (!observed) {
    return 8;
  }
  const auto identity = ytec::clonecore::validate_stable_identity(
      expected.value(), observed.value(), L"VM MBR2GPT marker disk");
  if (!identity) {
    std::wcerr << identity.error().operation << L": "
               << identity.error().message << L"\n";
    return 9;
  }

  std::array<std::byte, 512> sector{};
  std::memcpy(sector.data(), kMarker.data(), kMarker.size());
  DWORD transferred = 0;
  if (!WriteFile(
          handle.get(),
          sector.data(),
          static_cast<DWORD>(sector.size()),
          &transferred,
          nullptr) ||
      transferred != sector.size() || !FlushFileBuffers(handle.get())) {
    std::wcerr << L"Unable to write and flush the dedicated marker disk.\n";
    return 10;
  }
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(handle.get(), zero, nullptr, FILE_BEGIN)) {
    return 11;
  }
  std::array<std::byte, 512> read_back{};
  transferred = 0;
  if (!ReadFile(
          handle.get(),
          read_back.data(),
          static_cast<DWORD>(read_back.size()),
          &transferred,
          nullptr) ||
      transferred != read_back.size() || read_back != sector) {
    std::wcerr << L"Marker disk read-back verification failed.\n";
    return 12;
  }
  std::cout << kProbeStagedMarker << "\n";
  std::cout << kMarker << "\n";
  return 0;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc != 3 || std::wstring_view(argv[1]) != L"--authorization" ||
      std::wstring_view(argv[2]) != kAuthorization) {
    std::wcerr << L"The fixed VM-only authorization is required.\n";
    return 2;
  }
  if (!ytec::vmtest::is_virtualbox_guest() ||
      !ytec::vmtest::is_administrator()) {
    std::wcerr << L"This verifier runs only in an administrator WinPE VirtualBox guest.\n";
    return 3;
  }
  return write_verified_marker();
}

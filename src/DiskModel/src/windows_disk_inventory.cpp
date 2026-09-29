#include "ytec/diskmodel/disk_inventory.h"

#include "ytec/clonecore/log_privacy.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/inventory_formatter.h"
#include "system_disk.h"

#include <Windows.h>
#include <winioctl.h>
#include <nvme.h>
#include <SetupAPI.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::diskmodel {
namespace {

constexpr DWORD kInitialLayoutBufferSize = 64U * 1024U;
constexpr DWORD kMaximumQueryBufferSize = 1024U * 1024U;
constexpr DWORD kMaximumHealthQueryBufferSize = 64U * 1024U;

// DEVPKEY_Device_LocationPaths from the Windows SDK's devpkey.h. Keeping the
// one key local avoids a link-time dependency on SDK property-key definitions,
// which are not shipped as a Devpkey.lib in all supported build environments.
constexpr DEVPROPKEY kDeviceLocationPaths{
    {0xa45c254e,
     0xdf1c,
     0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    37U};

void append_issue(
    InventoryReport& report,
    std::wstring_view device,
    std::wstring_view operation,
    DWORD native_code);

std::wstring query_device_instance_id(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device_info,
    InventoryReport& report) {
  DWORD required_characters = 0;
  SetupDiGetDeviceInstanceIdW(
      devices, &device_info, nullptr, 0, &required_characters);
  const DWORD native_code = GetLastError();
  if (native_code != ERROR_INSUFFICIENT_BUFFER || required_characters == 0 ||
      required_characters > 32U * 1024U) {
    append_issue(
        report,
        L"SetupAPI disk interface",
        L"デバイスインスタンス識別子のサイズ取得",
        native_code);
    return {};
  }
  std::vector<wchar_t> buffer(required_characters, L'\0');
  if (!SetupDiGetDeviceInstanceIdW(
          devices,
          &device_info,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          nullptr)) {
    append_issue(
        report,
        L"SetupAPI disk interface",
        L"デバイスインスタンス識別子の取得",
        GetLastError());
    return {};
  }
  return std::wstring(buffer.data());
}

std::wstring query_connection_location_path(
    const HDEVINFO devices,
    SP_DEVINFO_DATA& device_info) {
  DEVPROPTYPE property_type = 0U;
  DWORD required_bytes = 0U;
  SetupDiGetDevicePropertyW(
      devices,
      &device_info,
      &kDeviceLocationPaths,
      &property_type,
      nullptr,
      0U,
      &required_bytes,
      0U);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      property_type != DEVPROP_TYPE_STRING_LIST ||
      required_bytes < 2U * sizeof(wchar_t) ||
      required_bytes > 32U * 1024U ||
      required_bytes % sizeof(wchar_t) != 0U) {
    return {};
  }

  std::vector<std::byte> buffer(required_bytes);
  if (!SetupDiGetDevicePropertyW(
          devices,
          &device_info,
          &kDeviceLocationPaths,
          &property_type,
          reinterpret_cast<PBYTE>(buffer.data()),
          static_cast<DWORD>(buffer.size()),
          nullptr,
          0U) ||
      property_type != DEVPROP_TYPE_STRING_LIST) {
    return {};
  }
  const auto* const locations =
      reinterpret_cast<const wchar_t*>(buffer.data());
  const std::size_t character_count = buffer.size() / sizeof(wchar_t);
  const auto terminator = std::find(
      locations, locations + character_count, L'\0');
  if (terminator == locations || terminator == locations + character_count) {
    return {};
  }
  return std::wstring(locations, terminator);
}

class DeviceInfoSet final {
 public:
  explicit DeviceInfoSet(HDEVINFO handle) noexcept : handle_(handle) {}
  ~DeviceInfoSet() noexcept {
    if (valid()) {
      SetupDiDestroyDeviceInfoList(handle_);
    }
  }

  DeviceInfoSet(const DeviceInfoSet&) = delete;
  DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }

 private:
  HDEVINFO handle_{INVALID_HANDLE_VALUE};
};

clonecore::ErrorCode query_error_code(const DWORD native_code) noexcept {
  return native_code == ERROR_ACCESS_DENIED ? clonecore::ErrorCode::access_denied
                                            : clonecore::ErrorCode::query_failed;
}

void append_issue(
    InventoryReport& report,
    const std::wstring_view device,
    const std::wstring_view operation,
    const DWORD native_code) {
  report.issues.push_back(InventoryIssue{
      .device = std::wstring(device),
      .error = clonecore::make_win32_error(
          query_error_code(native_code), operation, native_code),
  });
}

std::string trim_ascii(std::string value) {
  const auto is_space = [](const unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\0';
  };

  const auto first = std::find_if_not(value.begin(), value.end(), is_space);
  const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

std::string sanitize_storage_ascii(std::string value) {
  for (char& character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte > 0x7EU) {
      character = '?';
    }
  }
  return trim_ascii(std::move(value));
}

std::wstring ascii_to_wide(const std::string_view value) {
  std::wstring output;
  output.reserve(value.size());
  for (const unsigned char character : value) {
    output.push_back(static_cast<wchar_t>(character));
  }
  return output;
}

std::string storage_string(
    const std::vector<std::byte>& buffer,
    const DWORD offset) {
  if (offset == 0 || offset >= buffer.size()) {
    return {};
  }

  const char* const start =
      reinterpret_cast<const char*>(buffer.data() + offset);
  const std::size_t remaining = buffer.size() - offset;
  const void* const terminator = std::memchr(start, '\0', remaining);
  if (terminator == nullptr) {
    return {};
  }

  const auto* const end = static_cast<const char*>(terminator);
  return sanitize_storage_ascii(std::string(start, end));
}

std::wstring bus_type_name(const STORAGE_BUS_TYPE bus_type) {
  switch (static_cast<unsigned int>(bus_type)) {
    case 0:
      return L"Unknown";
    case 1:
      return L"SCSI";
    case 2:
      return L"ATAPI";
    case 3:
      return L"ATA";
    case 4:
      return L"IEEE 1394";
    case 5:
      return L"SSA";
    case 6:
      return L"Fibre Channel";
    case 7:
      return L"USB";
    case 8:
      return L"RAID";
    case 9:
      return L"iSCSI";
    case 10:
      return L"SAS";
    case 11:
      return L"SATA";
    case 12:
      return L"SD";
    case 13:
      return L"MMC";
    case 14:
      return L"Virtual";
    case 15:
      return L"File-backed virtual";
    case 16:
      return L"Storage Spaces";
    case 17:
      return L"NVMe";
    case 18:
      return L"SCM";
    case 19:
      return L"UFS";
    default:
      return L"Unknown(" + std::to_wstring(static_cast<unsigned int>(bus_type)) +
             L")";
  }
}

PartitionStyle map_partition_style(const PARTITION_STYLE style) noexcept {
  switch (style) {
    case PARTITION_STYLE_RAW:
      return PartitionStyle::raw;
    case PARTITION_STYLE_MBR:
      return PartitionStyle::mbr;
    case PARTITION_STYLE_GPT:
      return PartitionStyle::gpt;
    default:
      return PartitionStyle::unknown;
  }
}

std::wstring guid_to_string(const GUID& guid) {
  std::wostringstream stream;
  stream << L'{' << std::uppercase << std::hex << std::setfill(L'0')
         << std::setw(8) << guid.Data1 << L'-' << std::setw(4)
         << static_cast<unsigned int>(guid.Data2) << L'-' << std::setw(4)
         << static_cast<unsigned int>(guid.Data3) << L'-' << std::setw(2)
         << static_cast<unsigned int>(guid.Data4[0]) << std::setw(2)
         << static_cast<unsigned int>(guid.Data4[1]) << L'-';
  for (std::size_t index = 2; index < std::size(guid.Data4); ++index) {
    stream << std::setw(2) << static_cast<unsigned int>(guid.Data4[index]);
  }
  stream << L'}';
  return stream.str();
}

std::wstring mbr_type_to_string(const BYTE type) {
  std::wostringstream stream;
  stream << L"0x" << std::uppercase << std::hex << std::setw(2)
         << std::setfill(L'0') << static_cast<unsigned int>(type);
  return stream.str();
}

bool query_device_descriptor(
    const HANDLE handle,
    DiskInfo& disk,
    InventoryReport& report) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;

  STORAGE_DESCRIPTOR_HEADER header{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          &header,
          sizeof(header),
          &bytes_returned,
          nullptr)) {
    append_issue(
        report, disk.device_path, L"デバイス記述子のサイズ取得", GetLastError());
    return false;
  }

  if (header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
      header.Size > kMaximumQueryBufferSize) {
    append_issue(
        report,
        disk.device_path,
        L"デバイス記述子のサイズ検証",
        ERROR_INVALID_DATA);
    return false;
  }

  std::vector<std::byte> buffer(header.Size);
  bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr)) {
    append_issue(report, disk.device_path, L"デバイス記述子の取得", GetLastError());
    return false;
  }

  if (bytes_returned < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
      bytes_returned > buffer.size()) {
    append_issue(
        report,
        disk.device_path,
        L"デバイス記述子の長さ検証",
        ERROR_INVALID_DATA);
    return false;
  }

  buffer.resize(bytes_returned);

  const auto* const descriptor =
      reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
  const std::string vendor = storage_string(buffer, descriptor->VendorIdOffset);
  const std::string product = storage_string(buffer, descriptor->ProductIdOffset);
  const std::string model = trim_ascii(vendor + (vendor.empty() ? "" : " ") + product);
  disk.model = model.empty() ? L"未取得" : ascii_to_wide(model);
  disk.bus_type = bus_type_name(descriptor->BusType);
  disk.removable = descriptor->RemovableMedia != FALSE;
  const std::string serial =
      storage_string(buffer, descriptor->SerialNumberOffset);
  disk.serial_suffix = mask_serial_suffix(serial);
  disk.serial_log_token = clonecore::minimize_main_log_value(
      clonecore::MainLogPrivateValueKind::disk_serial,
      ascii_to_wide(serial));
  return true;
}

void query_failure_prediction(
    const HANDLE handle,
    DiskHealthObservation& observation) noexcept {
  STORAGE_PREDICT_FAILURE prediction{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_PREDICT_FAILURE,
          nullptr,
          0,
      &prediction,
      sizeof(prediction),
      &bytes_returned,
      nullptr) ||
      static_cast<std::size_t>(bytes_returned) <
          offsetof(STORAGE_PREDICT_FAILURE, VendorSpecific)) {
    // Failure prediction is optional and is not implemented by every storage
    // stack or USB bridge. Unsupported queries remain "unknown" rather than
    // turning a usable disk inventory into an error.
    return;
  }
  (void)parse_storage_predict_failure_response(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(&prediction),
          (std::min)(
              static_cast<std::size_t>(bytes_returned),
              sizeof(prediction))),
      observation);
}

void query_device_temperature(
    const HANDLE handle,
    DiskHealthObservation& observation) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceTemperatureProperty;
  query.QueryType = PropertyStandardQuery;

  STORAGE_DESCRIPTOR_HEADER header{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          &header,
          sizeof(header),
      &bytes_returned,
      nullptr) ||
      bytes_returned < sizeof(header) ||
      static_cast<std::size_t>(header.Size) <
          offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo) ||
      header.Size > kMaximumHealthQueryBufferSize) {
    return;
  }

  std::vector<std::byte> buffer(header.Size);
  bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr) ||
      bytes_returned < offsetof(
                           STORAGE_TEMPERATURE_DATA_DESCRIPTOR,
                           TemperatureInfo) ||
      bytes_returned > buffer.size()) {
    return;
  }

  (void)parse_storage_temperature_response(
      std::span<const std::byte>(buffer.data(), bytes_returned), observation);
}

void query_nvme_health(
    const HANDLE handle,
    DiskHealthObservation& observation) {
  constexpr std::size_t query_prefix =
      offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
  constexpr std::size_t input_size =
      query_prefix + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
  constexpr std::size_t protocol_descriptor_prefix =
      offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
  constexpr std::size_t output_size =
      protocol_descriptor_prefix + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
      sizeof(NVME_HEALTH_INFO_LOG);
  constexpr std::size_t buffer_size = std::max(input_size, output_size);
  static_assert(buffer_size <= std::numeric_limits<DWORD>::max());

  std::vector<std::byte> buffer(buffer_size);
  auto* const query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buffer.data());
  query->PropertyId = StorageDeviceProtocolSpecificProperty;
  query->QueryType = PropertyStandardQuery;
  auto* const request = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(
      buffer.data() + query_prefix);
  request->ProtocolType = ProtocolTypeNvme;
  request->DataType = NVMeDataTypeLogPage;
  request->ProtocolDataRequestValue =
      static_cast<DWORD>(NVME_LOG_PAGE_HEALTH_INFO);
  request->ProtocolDataOffset =
      static_cast<DWORD>(sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA));
  request->ProtocolDataLength =
      static_cast<DWORD>(sizeof(NVME_HEALTH_INFO_LOG));

  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          buffer.data(),
          static_cast<DWORD>(input_size),
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr) ||
      static_cast<std::size_t>(bytes_returned) < output_size ||
      static_cast<std::size_t>(bytes_returned) > buffer.size()) {
    return;
  }

  (void)parse_nvme_health_response(
      std::span<const std::byte>(buffer.data(), bytes_returned), observation);
}

void query_disk_health(const HANDLE handle, DiskInfo& disk) {
  DiskHealthObservation observation;
  query_failure_prediction(handle, observation);
  query_device_temperature(handle, observation);
  if (disk.bus_type == L"NVMe") {
    query_nvme_health(handle, observation);
  }
  disk.health = normalize_disk_health(observation);
}

void query_sector_alignment(
    const HANDLE handle,
    DiskInfo& disk,
    InventoryReport& report) {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageAccessAlignmentProperty;
  query.QueryType = PropertyStandardQuery;

  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          &alignment,
          sizeof(alignment),
          &bytes_returned,
          nullptr)) {
    append_issue(report, disk.device_path, L"セクターサイズの取得", GetLastError());
    return;
  }

  if (bytes_returned < sizeof(alignment)) {
    append_issue(
        report,
        disk.device_path,
        L"セクターサイズ情報の長さ検証",
        ERROR_INVALID_DATA);
    return;
  }

  disk.logical_sector_size = alignment.BytesPerLogicalSector;
  disk.physical_sector_size = alignment.BytesPerPhysicalSector;
}

void query_disk_length(
    const HANDLE handle,
    DiskInfo& disk,
    InventoryReport& report) {
  GET_LENGTH_INFORMATION length{};
  DWORD bytes_returned = 0;
  if (DeviceIoControl(
          handle,
          IOCTL_DISK_GET_LENGTH_INFO,
          nullptr,
          0,
          &length,
          sizeof(length),
          &bytes_returned,
          nullptr)) {
    if (bytes_returned >= sizeof(length) && length.Length.QuadPart >= 0) {
      disk.size_bytes = static_cast<std::uint64_t>(length.Length.QuadPart);
    } else {
      append_issue(
          report,
          disk.device_path,
          L"ディスク容量情報の検証",
          ERROR_INVALID_DATA);
      return;
    }
  } else {
    const DWORD length_error = GetLastError();
    DISK_GEOMETRY_EX geometry{};
    bytes_returned = 0;
    if (!DeviceIoControl(
            handle,
            IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr,
            0,
            &geometry,
            sizeof(geometry),
            &bytes_returned,
            nullptr) ||
        bytes_returned < offsetof(DISK_GEOMETRY_EX, Data) ||
        geometry.DiskSize.QuadPart < 0) {
      append_issue(
          report, disk.device_path, L"ディスク容量の取得", length_error);
      return;
    }

    disk.size_bytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
    if (disk.logical_sector_size == 0) {
      disk.logical_sector_size = geometry.Geometry.BytesPerSector;
    }
  }

  if (disk.logical_sector_size != 0) {
    disk.sector_count = disk.size_bytes / disk.logical_sector_size;
  }
}

void query_disk_attributes(
    const HANDLE handle,
    DiskInfo& disk,
    InventoryReport& report) {
  GET_DISK_ATTRIBUTES attributes{};
  attributes.Version = sizeof(attributes);

  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_DISK_GET_DISK_ATTRIBUTES,
          nullptr,
          0,
          &attributes,
          sizeof(attributes),
          &bytes_returned,
          nullptr)) {
    append_issue(report, disk.device_path, L"ディスク属性の取得", GetLastError());
    return;
  }

  if (bytes_returned < sizeof(attributes)) {
    append_issue(
        report,
        disk.device_path,
        L"ディスク属性情報の長さ検証",
        ERROR_INVALID_DATA);
    return;
  }

  disk.offline = (attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
  disk.read_only = (attributes.Attributes & DISK_ATTRIBUTE_READ_ONLY) != 0;
}

void populate_partition(
    const PARTITION_INFORMATION_EX& source,
    PartitionInfo& destination) {
  destination.number = source.PartitionNumber;
  destination.offset_bytes = source.StartingOffset.QuadPart < 0
                                 ? 0
                                 : static_cast<std::uint64_t>(
                                       source.StartingOffset.QuadPart);
  destination.size_bytes = source.PartitionLength.QuadPart < 0
                               ? 0
                               : static_cast<std::uint64_t>(
                                     source.PartitionLength.QuadPart);
  destination.style = map_partition_style(source.PartitionStyle);

  if (source.PartitionStyle == PARTITION_STYLE_GPT) {
    destination.type = guid_to_string(source.Gpt.PartitionType);
    destination.identifier = guid_to_string(source.Gpt.PartitionId);
    const std::size_t name_length =
        wcsnlen(source.Gpt.Name, static_cast<std::size_t>(std::size(source.Gpt.Name)));
    destination.name.assign(source.Gpt.Name, name_length);
    for (wchar_t& character : destination.name) {
      if (character < L' ') {
        character = L'?';
      }
    }
  } else if (source.PartitionStyle == PARTITION_STYLE_MBR) {
    destination.type = mbr_type_to_string(source.Mbr.PartitionType);
    destination.bootable = source.Mbr.BootIndicator != FALSE;
  } else {
    destination.type = L"RAW";
  }
}

void query_drive_layout(
    const HANDLE handle,
    DiskInfo& disk,
    InventoryReport& report) {
  DWORD buffer_size = kInitialLayoutBufferSize;
  std::vector<std::byte> buffer(buffer_size);
  DWORD bytes_returned = 0;

  while (!DeviceIoControl(
      handle,
      IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
      nullptr,
      0,
      buffer.data(),
      buffer_size,
      &bytes_returned,
      nullptr)) {
    const DWORD native_code = GetLastError();
    if ((native_code == ERROR_INSUFFICIENT_BUFFER || native_code == ERROR_MORE_DATA) &&
        buffer_size < kMaximumQueryBufferSize) {
      buffer_size = std::min(buffer_size * 2U, kMaximumQueryBufferSize);
      buffer.resize(buffer_size);
      continue;
    }
    append_issue(report, disk.device_path, L"パーティション一覧の取得", native_code);
    return;
  }

  constexpr std::size_t header_size =
      offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
  if (bytes_returned < header_size || bytes_returned > buffer.size()) {
    append_issue(
        report,
        disk.device_path,
        L"パーティション一覧の長さ検証",
        ERROR_INVALID_DATA);
    return;
  }

  const auto* const layout =
      reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
  const std::size_t available_entries =
      (static_cast<std::size_t>(bytes_returned) - header_size) /
      sizeof(PARTITION_INFORMATION_EX);
  if (layout->PartitionCount > available_entries) {
    append_issue(
        report,
        disk.device_path,
        L"パーティション件数の境界検証",
        ERROR_INVALID_DATA);
    return;
  }

  disk.partition_style = normalize_disk_partition_style(
      map_partition_style(static_cast<PARTITION_STYLE>(layout->PartitionStyle)),
      layout->PartitionCount);
  if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
    disk.disk_identifier = guid_to_string(layout->Gpt.DiskId);
  } else if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
    std::wostringstream identifier;
    identifier << L"0x" << std::uppercase << std::hex << std::setw(8)
               << std::setfill(L'0') << layout->Mbr.Signature;
    disk.disk_identifier = identifier.str();
  } else {
    disk.disk_identifier = L"RAW";
  }
  disk.partitions.reserve(layout->PartitionCount);
  for (DWORD index = 0; index < layout->PartitionCount; ++index) {
    const PARTITION_INFORMATION_EX& entry = layout->PartitionEntry[index];
    if (entry.PartitionNumber == 0 || entry.PartitionLength.QuadPart <= 0) {
      continue;
    }
    PartitionInfo partition;
    populate_partition(entry, partition);
    disk.partitions.push_back(std::move(partition));
  }
}

clonecore::Result<std::uint32_t> query_disk_number(const HANDLE handle) {
  STORAGE_DEVICE_NUMBER device_number{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_GET_DEVICE_NUMBER,
          nullptr,
          0,
          &device_number,
          sizeof(device_number),
          &bytes_returned,
          nullptr)) {
    const DWORD native_code = GetLastError();
    return clonecore::Result<std::uint32_t>::failure(clonecore::make_win32_error(
        query_error_code(native_code), L"ディスク番号の取得", native_code));
  }

  if (bytes_returned < sizeof(device_number) ||
      device_number.DeviceType != FILE_DEVICE_DISK) {
    return clonecore::Result<std::uint32_t>::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"ディスク番号情報の検証",
        ERROR_INVALID_DATA));
  }

  return clonecore::Result<std::uint32_t>::success(device_number.DeviceNumber);
}

class WindowsDiskInventoryProvider final : public IDiskInventoryProvider {
 public:
  explicit WindowsDiskInventoryProvider(const clonecore::Logger* logger)
      : logger_(logger) {}

  clonecore::Result<InventoryReport> enumerate() override {
    if (logger_ != nullptr) {
      logger_->info(L"読み取り専用の物理ディスク列挙を開始します");
    }

    DeviceInfoSet devices(SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_DISK,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!devices.valid()) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<InventoryReport>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"ディスクデバイス一覧の開始",
              native_code));
    }

    InventoryReport report;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA interface_data{};
      interface_data.cbSize = sizeof(interface_data);
      if (!SetupDiEnumDeviceInterfaces(
              devices.get(),
              nullptr,
              &GUID_DEVINTERFACE_DISK,
              index,
              &interface_data)) {
        const DWORD native_code = GetLastError();
        if (native_code == ERROR_NO_MORE_ITEMS) {
          break;
        }
        return clonecore::Result<InventoryReport>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::enumeration_failed,
                L"ディスクデバイス一覧の反復",
                native_code));
      }

      DWORD required_size = 0;
      SetupDiGetDeviceInterfaceDetailW(
          devices.get(),
          &interface_data,
          nullptr,
          0,
          &required_size,
          nullptr);
      const DWORD size_error = GetLastError();
      if (required_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) ||
          required_size > kMaximumQueryBufferSize ||
          size_error != ERROR_INSUFFICIENT_BUFFER) {
        append_issue(
            report,
            L"SetupAPI disk interface",
            L"デバイスパスのサイズ取得",
            size_error);
        continue;
      }

      std::vector<std::byte> detail_buffer(required_size);
      auto* const detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
          detail_buffer.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      SP_DEVINFO_DATA device_info{};
      device_info.cbSize = sizeof(device_info);

      if (!SetupDiGetDeviceInterfaceDetailW(
              devices.get(),
              &interface_data,
              detail,
              required_size,
              nullptr,
              &device_info)) {
        append_issue(
            report,
            L"SetupAPI disk interface",
            L"デバイスパスの取得",
            GetLastError());
        continue;
      }

      const std::wstring interface_path(detail->DevicePath);
      clonecore::UniqueHandle handle(CreateFileW(
          interface_path.c_str(),
          0,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL,
          nullptr));
      if (!handle) {
        append_issue(
            report, L"物理ディスク", L"照会専用ハンドルのオープン", GetLastError());
        continue;
      }

      auto disk_number = query_disk_number(handle.get());
      if (!disk_number) {
        report.issues.push_back(InventoryIssue{
            .device = L"物理ディスク",
            .error = disk_number.error(),
        });
        continue;
      }

      DiskInfo disk;
      disk.disk_number = disk_number.value();
      disk.device_path =
          L"\\\\.\\PhysicalDrive" + std::to_wstring(disk.disk_number);
      disk.device_interface_path = interface_path;
      disk.connection_location_path =
          query_connection_location_path(devices.get(), device_info);
      disk.device_instance_id =
          query_device_instance_id(devices.get(), device_info, report);
      disk.model = L"未取得";
      disk.bus_type = L"Unknown";

      query_device_descriptor(handle.get(), disk, report);
      query_disk_health(handle.get(), disk);
      query_sector_alignment(handle.get(), disk, report);
      query_disk_length(handle.get(), disk, report);
      query_disk_attributes(handle.get(), disk, report);
      query_drive_layout(handle.get(), disk, report);
      report.disks.push_back(std::move(disk));
    }

    std::sort(
        report.disks.begin(),
        report.disks.end(),
        [](const DiskInfo& left, const DiskInfo& right) {
          return left.disk_number < right.disk_number;
        });

    const auto system_disks = query_windows_system_disk_numbers();
    if (!system_disks) {
      report.issues.push_back(InventoryIssue{
          .device = L"実行中Windows",
          .error = system_disks.error(),
      });
    } else {
      for (auto& disk : report.disks) {
        disk.is_system_disk = std::find(
            system_disks.value().begin(),
            system_disks.value().end(),
            disk.disk_number) != system_disks.value().end();
      }
    }

    if (logger_ != nullptr) {
      logger_->info(
          L"読み取り専用の物理ディスク列挙を終了しました（" +
          std::to_wstring(report.disks.size()) + L"台）");
    }
    return clonecore::Result<InventoryReport>::success(std::move(report));
  }

 private:
  const clonecore::Logger* logger_{};
};

}  // namespace

std::unique_ptr<IDiskInventoryProvider> make_windows_disk_inventory_provider(
    const clonecore::Logger* logger) {
  return std::make_unique<WindowsDiskInventoryProvider>(logger);
}

}  // namespace ytec::diskmodel

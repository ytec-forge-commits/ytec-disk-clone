#include "ytec/diskmodel/inventory_formatter.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ytec::diskmodel {
namespace {

std::string bool_or_null(const std::optional<bool> value) {
  if (!value.has_value()) {
    return "null";
  }
  return *value ? "true" : "false";
}

std::string human_bytes(const std::uint64_t bytes) {
  constexpr double gibibyte = 1024.0 * 1024.0 * 1024.0;
  std::ostringstream stream;
  stream << bytes << " bytes (" << std::fixed << std::setprecision(2)
         << static_cast<double>(bytes) / gibibyte << " GiB)";
  return stream.str();
}

std::string number_or_unknown(const std::uint32_t value) {
  return value == 0 ? "未取得" : std::to_string(value);
}

template <typename Number>
std::string optional_number_or_null(const std::optional<Number>& value) {
  return value.has_value()
             ? std::to_string(static_cast<long long>(value.value()))
             : "null";
}

std::string error_to_json(const InventoryIssue& issue) {
  std::ostringstream stream;
  stream << "{\"device\":\"" << json_escape(to_utf8(issue.device))
         << "\",\"code\":\""
         << json_escape(to_utf8(clonecore::error_code_name(issue.error.code)))
         << "\",\"nativeCode\":" << issue.error.native_code
         << ",\"operation\":\""
         << json_escape(to_utf8(issue.error.operation)) << "\",\"message\":\""
         << json_escape(to_utf8(issue.error.message)) << "\"}";
  return stream.str();
}

}  // namespace

std::wstring_view partition_style_name(const PartitionStyle style) noexcept {
  switch (style) {
    case PartitionStyle::raw:
      return L"RAW";
    case PartitionStyle::mbr:
      return L"MBR";
    case PartitionStyle::gpt:
      return L"GPT";
    case PartitionStyle::unknown:
      return L"UNKNOWN";
  }
  return L"UNKNOWN";
}

PartitionStyle normalize_disk_partition_style(
    const PartitionStyle reported_style,
    const std::size_t partition_count) noexcept {
  // IOCTL_DISK_GET_DRIVE_LAYOUT_EX can report the zero-valued native MBR enum
  // for an uninitialized disk. With no partition entries, the observable disk
  // state is RAW regardless of that placeholder value.
  return partition_count == 0 ? PartitionStyle::raw : reported_style;
}

std::string to_utf8(const std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return "<文字列が長すぎます>";
  }

  const int input_length = static_cast<int>(value.size());
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      input_length,
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return "<無効なUnicode>";
  }

  std::string output(static_cast<std::size_t>(required), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      input_length,
      output.data(),
      required,
      nullptr,
      nullptr);
  if (written != required) {
    return "<Unicode変換失敗>";
  }
  return output;
}

std::string json_escape(const std::string_view value) {
  std::ostringstream escaped;
  escaped << std::hex << std::setfill('0');
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        escaped << "\\\"";
        break;
      case '\\':
        escaped << "\\\\";
        break;
      case '\b':
        escaped << "\\b";
        break;
      case '\f':
        escaped << "\\f";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        if (character < 0x20U) {
          escaped << "\\u" << std::setw(4)
                  << static_cast<unsigned int>(character);
        } else {
          escaped << static_cast<char>(character);
        }
        break;
    }
  }
  return escaped.str();
}

std::string mask_serial_suffix(const std::string_view serial) {
  std::string printable;
  printable.reserve(serial.size());
  for (const unsigned char character : serial) {
    if (character >= 0x20U && character <= 0x7EU) {
      printable.push_back(static_cast<char>(character));
    }
  }

  const auto first = std::find_if_not(printable.begin(), printable.end(), [](const char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
  });
  const auto last =
      std::find_if_not(printable.rbegin(), printable.rend(), [](const char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
      }).base();
  if (first >= last) {
    return {};
  }

  const std::string_view trimmed(
      &*first,
      static_cast<std::size_t>(std::distance(first, last)));
  constexpr std::size_t maximum_suffix_length = 8;
  if (trimmed.size() <= maximum_suffix_length) {
    return std::string(trimmed);
  }
  return std::string(trimmed.substr(trimmed.size() - maximum_suffix_length));
}

std::string inventory_to_json(const InventoryReport& report) {
  std::ostringstream stream;
  stream << "{\"schemaVersion\":1,\"mode\":\"read-only\",\"disks\":[";

  for (std::size_t disk_index = 0; disk_index < report.disks.size(); ++disk_index) {
    if (disk_index != 0) {
      stream << ',';
    }
    const DiskInfo& disk = report.disks[disk_index];
    stream << "{\"diskNumber\":" << disk.disk_number << ",\"devicePath\":\""
           << json_escape(to_utf8(disk.device_path)) << "\",\"model\":\""
           << json_escape(to_utf8(disk.model)) << "\",\"sizeBytes\":"
           << disk.size_bytes << ",\"sectorCount\":" << disk.sector_count
           << ",\"logicalSectorSize\":" << disk.logical_sector_size
           << ",\"physicalSectorSize\":" << disk.physical_sector_size
           << ",\"busType\":\"" << json_escape(to_utf8(disk.bus_type))
           << "\",\"serialSuffix\":\"" << json_escape(disk.serial_suffix)
           << "\",\"partitionStyle\":\""
           << json_escape(to_utf8(partition_style_name(disk.partition_style)))
           << "\",\"offline\":" << bool_or_null(disk.offline)
           << ",\"readOnly\":" << bool_or_null(disk.read_only)
           << ",\"removable\":" << bool_or_null(disk.removable)
           << ",\"health\":{\"state\":\""
           << json_escape(to_utf8(disk_health_state_name(disk.health.state)))
           << "\",\"smartStatusAvailable\":"
           << (disk.health.smart_status_available ? "true" : "false")
           << ",\"nvmeHealthAvailable\":"
           << (disk.health.nvme_health_available ? "true" : "false")
           << ",\"temperatureWarning\":"
           << (disk.health.temperature_warning ? "true" : "false")
           << ",\"temperatureCelsius\":"
           << optional_number_or_null(disk.health.temperature_celsius)
           << ",\"warningTemperatureCelsius\":"
           << optional_number_or_null(
                  disk.health.warning_temperature_celsius)
           << ",\"criticalTemperatureCelsius\":"
           << optional_number_or_null(
                  disk.health.critical_temperature_celsius)
           << ",\"nvmeAvailableSparePercent\":"
           << optional_number_or_null(
                  disk.health.nvme_available_spare_percent)
           << ",\"nvmePercentageUsed\":"
           << optional_number_or_null(disk.health.nvme_percentage_used)
           << ",\"nvmeCriticalWarning\":"
           << static_cast<unsigned int>(
                  disk.health.nvme_critical_warning)
           << "}"
           << ",\"partitions\":[";

    for (std::size_t partition_index = 0;
         partition_index < disk.partitions.size();
         ++partition_index) {
      if (partition_index != 0) {
        stream << ',';
      }
      const PartitionInfo& partition = disk.partitions[partition_index];
      stream << "{\"number\":" << partition.number << ",\"offsetBytes\":"
             << partition.offset_bytes << ",\"sizeBytes\":"
             << partition.size_bytes << ",\"style\":\""
             << json_escape(to_utf8(partition_style_name(partition.style)))
             << "\",\"type\":\"" << json_escape(to_utf8(partition.type))
             << "\",\"identifier\":\""
             << json_escape(to_utf8(partition.identifier)) << "\",\"name\":\""
             << json_escape(to_utf8(partition.name)) << "\",\"bootable\":"
             << (partition.bootable ? "true" : "false") << '}';
    }
    stream << "]}";
  }

  stream << "],\"issues\":[";
  for (std::size_t issue_index = 0; issue_index < report.issues.size(); ++issue_index) {
    if (issue_index != 0) {
      stream << ',';
    }
    stream << error_to_json(report.issues[issue_index]);
  }
  stream << "]}\n";
  return stream.str();
}

std::string inventory_to_text(const InventoryReport& report) {
  std::ostringstream stream;
  stream << "Y-TEC ディスク診断（読み取り専用）\n"
         << "検出ディスク数: " << report.disks.size() << "\n";

  for (const DiskInfo& disk : report.disks) {
    stream << "\nディスク " << disk.disk_number << "\n"
           << "  デバイス: " << to_utf8(disk.device_path) << "\n"
           << "  モデル: " << to_utf8(disk.model) << "\n"
           << "  容量: " << human_bytes(disk.size_bytes) << "\n"
           << "  セクター数: " << disk.sector_count << "\n"
           << "  論理セクター: " << number_or_unknown(disk.logical_sector_size)
           << " bytes\n"
           << "  物理セクター: " << number_or_unknown(disk.physical_sector_size)
           << " bytes\n"
           << "  BusType: " << to_utf8(disk.bus_type) << "\n"
           << "  シリアル末尾: "
           << (disk.serial_suffix.empty() ? "未取得" : disk.serial_suffix) << "\n"
           << "  パーティション形式: "
           << to_utf8(partition_style_name(disk.partition_style)) << "\n"
           << "  健康状態: "
           << to_utf8(disk_health_state_name(disk.health.state)) << "\n"
           << "  温度: "
           << (disk.health.temperature_celsius.has_value()
                   ? std::to_string(disk.health.temperature_celsius.value()) +
                         " C" +
                         (disk.health.temperature_warning ? " (警告)" : "")
                   : "未取得")
           << "\n"
           << "  パーティション数: " << disk.partitions.size() << "\n";

    for (const PartitionInfo& partition : disk.partitions) {
      stream << "    - #" << partition.number << "  オフセット="
             << partition.offset_bytes << "  サイズ=" << partition.size_bytes
             << "  種別=" << to_utf8(partition.type);
      if (!partition.name.empty()) {
        stream << "  名前=" << to_utf8(partition.name);
      }
      if (!partition.identifier.empty()) {
        stream << "  ID=" << to_utf8(partition.identifier);
      }
      if (partition.bootable) {
        stream << "  起動フラグあり";
      }
      stream << '\n';
    }
  }

  if (!report.issues.empty()) {
    stream << "\n診断上の未取得・警告: " << report.issues.size() << "件\n";
    for (const InventoryIssue& issue : report.issues) {
      stream << "  - " << to_utf8(issue.device) << ": "
             << to_utf8(issue.error.operation) << " (Windows error "
             << issue.error.native_code << ") " << to_utf8(issue.error.message)
             << '\n';
    }
  }

  return stream.str();
}

}  // namespace ytec::diskmodel

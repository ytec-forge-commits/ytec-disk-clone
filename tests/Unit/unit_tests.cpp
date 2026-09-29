#include "ytec/clonecore/result.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/log.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/inventory_formatter.h"

#include <Windows.h>

#include <array>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

void test_json_escape() {
  check(
      ytec::diskmodel::json_escape("quote=\" slash=\\ line=\n") ==
          "quote=\\\" slash=\\\\ line=\\n",
      "JSON escaping must cover quotes, slashes, and control characters");
}

void test_serial_masking() {
  check(
      ytec::diskmodel::mask_serial_suffix("  ABCD-1234-5678  ") == "234-5678",
      "Only the final eight serial characters may be retained");
  check(
      ytec::diskmodel::mask_serial_suffix("  1234 ") == "1234",
      "Short serials should be trimmed");
  check(
      ytec::diskmodel::mask_serial_suffix("   ").empty(),
      "Blank serials should not be reported");
  check(
      ytec::diskmodel::mask_serial_suffix("ABC\nDEF\x1b[31m1234") == "[31m1234",
      "Serial suffixes must remove terminal control characters");
}

ytec::diskmodel::InventoryReport sample_report() {
  ytec::diskmodel::PartitionInfo partition;
  partition.number = 1;
  partition.offset_bytes = 1'048'576;
  partition.size_bytes = 10'000'000;
  partition.style = ytec::diskmodel::PartitionStyle::gpt;
  partition.type = L"{type-guid}";
  partition.identifier = L"{partition-guid}";
  partition.name = L"Windows \"OS\"";

  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 3;
  disk.device_path = L"\\\\.\\PhysicalDrive3";
  disk.model = L"Mock NVMe";
  disk.size_bytes = 20'000'000;
  disk.sector_count = 39'062;
  disk.logical_sector_size = 512;
  disk.physical_sector_size = 4096;
  disk.bus_type = L"NVMe";
  disk.serial_suffix = "ABCD1234";
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.offline = false;
  disk.read_only = true;
  disk.removable = false;
  disk.health.state = ytec::diskmodel::DiskHealthState::healthy;
  disk.health.smart_status_available = true;
  disk.health.temperature_celsius = 42;
  disk.partitions.push_back(std::move(partition));

  ytec::diskmodel::InventoryReport report;
  report.disks.push_back(std::move(disk));
  return report;
}

void test_json_inventory() {
  const std::string json = ytec::diskmodel::inventory_to_json(sample_report());
  check(json.find("\"mode\":\"read-only\"") != std::string::npos,
        "JSON must declare read-only mode");
  check(json.find("\"diskNumber\":3") != std::string::npos,
        "JSON must include the disk number");
  check(json.find("\"physicalSectorSize\":4096") != std::string::npos,
        "JSON must include the physical sector size");
  check(json.find("Windows \\\"OS\\\"") != std::string::npos,
        "JSON must escape partition names");
  check(json.find("ABCD1234") != std::string::npos,
        "JSON must include only the supplied serial suffix");
  check(json.find("\"state\":\"警告なし\"") != std::string::npos,
        "JSON must include normalized health state");
  check(json.find("\"temperatureCelsius\":42") != std::string::npos,
        "JSON must include the reported temperature");
}

void test_text_inventory() {
  const std::string text = ytec::diskmodel::inventory_to_text(sample_report());
  check(text.find("読み取り専用") != std::string::npos,
        "Text output must clearly state read-only mode");
  check(text.find("ディスク 3") != std::string::npos,
        "Text output must include the disk number");
  check(text.find("NVMe") != std::string::npos,
        "Text output must include the bus type");
  check(text.find("健康状態: 警告なし") != std::string::npos,
        "Text output must include device health");
  check(text.find("温度: 42 C") != std::string::npos,
        "Text output must include device temperature");
}

void test_empty_drive_layout_is_raw() {
  check(
      ytec::diskmodel::normalize_disk_partition_style(
          ytec::diskmodel::PartitionStyle::mbr, 0) ==
          ytec::diskmodel::PartitionStyle::raw,
      "A zero-partition Windows layout must be reported as RAW");
  check(
      ytec::diskmodel::normalize_disk_partition_style(
          ytec::diskmodel::PartitionStyle::gpt, 4) ==
          ytec::diskmodel::PartitionStyle::gpt,
      "A populated GPT layout must remain GPT");
}

void test_unique_handle_move() {
  HANDLE event_handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event_handle == nullptr || event_handle == INVALID_HANDLE_VALUE) {
    throw TestFailure{"A test event handle should be created"};
  }

  ytec::clonecore::UniqueHandle first(event_handle);
  ytec::clonecore::UniqueHandle second(std::move(first));
  check(!first.valid(), "Moved-from handles must be invalid");
  check(second.get() == event_handle, "Moved-to handles must own the resource");
}

void test_result_error() {
  ytec::clonecore::Error expected{
      .code = ytec::clonecore::ErrorCode::query_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = L"test",
      .message = L"invalid",
  };
  const auto result =
      ytec::clonecore::Result<int>::failure(std::move(expected));
  check(!result.has_value(), "Failed results must not contain a value");
  check(result.error().native_code == ERROR_INVALID_DATA,
        "Failed results must retain the native error code");
}

void test_windows_guid_generator_creates_uuid_v4_layout() {
  auto generator = ytec::clonecore::make_windows_guid_generator();
  const auto first = generator->next_guid();
  const auto second = generator->next_guid();

  check(first.has_value() && second.has_value(),
        "Windows CNG should generate GPT GUIDs");
  check(!first.value().is_zero() && !second.value().is_zero(),
        "Generated GPT GUIDs must not be zero");
  check(first.value() != second.value(),
        "Consecutive GPT GUIDs should be unique");
  check((first.value().bytes[7] & std::byte{0xF0}) == std::byte{0x40},
        "Generated GPT GUIDs must use UUID version 4");
  check((first.value().bytes[8] & std::byte{0xC0}) == std::byte{0x80},
        "Generated GPT GUIDs must use the RFC 4122 variant");
}

void test_stable_identity_requires_multiple_signals() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 9;
  disk.model = L"Synthetic Disk";
  disk.size_bytes = 10'000'000;
  disk.logical_sector_size = 512;
  disk.device_instance_id = L"TEST\\SYNTHETIC_DISK";
  const auto identity = ytec::diskmodel::make_stable_disk_identity(disk, false);
  check(identity.has_value(), "A complete stable identity should be accepted");
  check(
      identity.value().device_instance_id == disk.device_instance_id,
      "The device instance identifier must be retained internally");

  disk.device_instance_id.clear();
  const auto incomplete =
      ytec::diskmodel::make_stable_disk_identity(disk, false);
  check(
      !incomplete.has_value(),
      "Disk number, model, and size alone must not form a stable identity");
}

void test_utf8_file_logger_is_new_single_line_and_non_throwing() {
  std::array<wchar_t, MAX_PATH + 1U> temporary{};
  const DWORD length = GetTempPathW(
      static_cast<DWORD>(temporary.size()), temporary.data());
  check(
      length > 0U && length < temporary.size(),
      "A temporary path should be available");
  const std::wstring path =
      std::wstring(temporary.data(), length) +
      L"ytec-log-test-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(GetTickCount64()) + L".log";
  {
    const auto logger =
        ytec::clonecore::make_utf8_file_logger(path, false);
    check(logger.has_value(), "A new UTF-8 log should be created");
    logger.value().info(L"日本語メッセージ\n改行");
    logger.value().debug(L"DEBUG_SHOULD_NOT_APPEAR");

    const auto duplicate =
        ytec::clonecore::make_utf8_file_logger(path, true);
    check(!duplicate.has_value(), "An existing log must not be overwritten");

    const ytec::clonecore::Logger throwing(
        [](const ytec::clonecore::LogRecord&) {
          throw TestFailure{"Logger sink failure"};
        });
    throwing.error(L"A throwing sink must be contained");
  }

  std::ifstream stream(path, std::ios::binary);
  const std::string contents{
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>()};
  check(
      contents.starts_with("\xEF\xBB\xBF"),
      "The log should start with a UTF-8 BOM");
  check(
      contents.find("DEBUG_SHOULD_NOT_APPEAR") == std::string::npos,
      "Disabled DEBUG records must not be written");
  check(
      contents.find('\n') == contents.rfind('\n'),
      "Embedded newlines must be sanitized into one record line");
  stream.close();
  check(
      DeleteFileW(path.c_str()) != FALSE,
      "The exact temporary test log should be removable");
}

void test_bounded_ram_logger_sanitizes_and_evicts() {
  ytec::clonecore::BoundedRamLogRouter router({
      .maximum_records = 2U,
      .maximum_total_message_characters = 16U,
      .maximum_message_characters = 8U,
  });
  const auto logger = router.logger();
  std::wstring controls = L"one\n two\t";
  controls.push_back(L'\0');
  controls += L"tail";
  logger.info(controls);
  logger.warning(L"second-record");
  logger.error(L"third");

  const auto snapshot = router.snapshot();
  check(snapshot.records.size() <= 2U,
        "The RAM log must retain at most the configured record count");
  check(snapshot.stored_message_characters <= 16U,
        "The RAM log must retain at most the configured character count");
  check(snapshot.dropped_record_count >= 1U,
        "Evicted RAM records must be counted");
  for (const auto& record : snapshot.records) {
    check(record.message.size() <= 8U,
          "Each RAM log message must respect its configured bound");
    check(record.message.find_first_of(L"\r\n\t") == std::wstring::npos &&
              record.message.find(L'\0') == std::wstring::npos,
          "RAM log records must be single-line sanitized");
  }
}

void test_bounded_ram_logger_file_to_ram_is_irreversible_for_all_copies() {
  struct SinkLifetime final {
    explicit SinkLifetime(std::vector<std::wstring>& destination)
        : records(destination) {}
    std::vector<std::wstring>& records;
  };

  ytec::clonecore::BoundedRamLogRouter router;
  const auto worker_copy = router.logger();
  std::vector<std::wstring> persistent_records;
  auto owner = std::make_shared<SinkLifetime>(persistent_records);
  const std::weak_ptr<SinkLifetime> lifetime = owner;
  check(router.attach_persistent_sink(ytec::clonecore::Logger(
            [owner](const ytec::clonecore::LogRecord& record) {
              owner->records.push_back(record.message);
            })),
        "A fresh RAM router should accept one persistent sink");
  owner.reset();
  worker_copy.info(L"before isolation");
  check(persistent_records.size() == 1U && !lifetime.expired(),
        "Worker logger copies must route to the attached sink");

  router.isolate_to_ram_permanently();
  check(lifetime.expired(),
        "Permanent RAM isolation must release the only persistent sink owner");
  worker_copy.info(L"after isolation");
  check(persistent_records.size() == 1U,
        "Existing worker logger copies must stop persistent writes");
  check(router.permanently_ram_only() &&
            !router.persistent_sink_attached(),
        "The router must report the irreversible RAM-only state");

  ytec::clonecore::Logger later_sink(
      [&persistent_records](const ytec::clonecore::LogRecord& record) {
        persistent_records.push_back(record.message);
      });
  check(!router.attach_persistent_sink(std::move(later_sink)),
        "A permanently isolated router must never reattach a file sink");
  const auto snapshot = router.snapshot();
  check(snapshot.records.size() == 2U &&
            snapshot.records.back().message == L"after isolation",
        "RAM logging must continue after persistent isolation");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"json_escape", test_json_escape},
      {"serial_masking", test_serial_masking},
      {"json_inventory", test_json_inventory},
      {"text_inventory", test_text_inventory},
      {"empty_drive_layout_is_raw", test_empty_drive_layout_is_raw},
      {"unique_handle_move", test_unique_handle_move},
      {"result_error", test_result_error},
      {"windows_guid_generator_creates_uuid_v4_layout",
       test_windows_guid_generator_creates_uuid_v4_layout},
      {"stable_identity_requires_multiple_signals",
       test_stable_identity_requires_multiple_signals},
      {"utf8_file_logger_is_new_single_line_and_non_throwing",
       test_utf8_file_logger_is_new_single_line_and_non_throwing},
      {"bounded_ram_logger_sanitizes_and_evicts",
       test_bounded_ram_logger_sanitizes_and_evicts},
      {"bounded_ram_logger_file_to_ram_is_irreversible_for_all_copies",
       test_bounded_ram_logger_file_to_ram_is_irreversible_for_all_copies},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }

  return failures == 0 ? 0 : 1;
}

#include "ytec/bootrepair/nvram_repair.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::bootrepair::CurrentPcNvramRepairRequest;
using ytec::bootrepair::FirmwareVariableValue;
using ytec::clonecore::Error;
using ytec::clonecore::ErrorCode;
using ytec::clonecore::Result;
using ytec::clonecore::Status;

int failures = 0;

void check(const bool condition, const char* const message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

Error test_error(const wchar_t* const operation) {
  return Error{
      .code = ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = operation,
      .message = L"synthetic failure",
  };
}

template <typename T>
void append_le(std::vector<std::byte>& bytes, const T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes.push_back(static_cast<std::byte>(
        (value >> (index * 8U)) & static_cast<T>(0xFFU)));
  }
}

FirmwareVariableValue boot_order(
    const std::initializer_list<std::uint16_t> numbers) {
  FirmwareVariableValue value{.attributes = 7U};
  for (const auto number : numbers) {
    append_le(value.bytes, number);
  }
  return value;
}

ytec::diskmodel::PartitionInfo esp(
    const std::uint32_t number = 1U,
    const wchar_t* const identifier =
        L"{11111111-2222-3333-4444-555555555555}") {
  return ytec::diskmodel::PartitionInfo{
      .number = number,
      .offset_bytes = 2048ULL * 512ULL,
      .size_bytes = 200ULL * 1024ULL * 1024ULL,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
      .identifier = identifier,
      .name = L"EFI system partition",
      .bootable = true,
  };
}

CurrentPcNvramRepairRequest valid_request() {
  return CurrentPcNvramRepairRequest{
      .expected_disk = ytec::clonecore::StableDiskIdentity{
          .disk_number = 3U,
          .model = L"Synthetic GPT disk",
          .size_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL,
          .logical_sector_size = 512U,
          .serial_suffix = "A1B2",
          .device_instance_id = L"SYNTHETIC\\DISK3",
          .is_system_disk = false,
      },
      .expected_esp = esp(),
      .logical_sector_size = 512U,
      .explicitly_for_current_pc = true,
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
  };
}

class MockPlatform final
    : public ytec::bootrepair::ICurrentPcNvramRepairPlatform {
 public:
  Status revalidate_target(
      const ytec::clonecore::StableDiskIdentity&,
      const ytec::diskmodel::PartitionInfo&) override {
    ++revalidate_calls;
    if (fail_revalidate_at != 0U && revalidate_calls == fail_revalidate_at) {
      return Status::failure(test_error(L"target revalidate"));
    }
    return ytec::clonecore::success_status();
  }

  Result<std::optional<FirmwareVariableValue>> read_efi_global_variable(
      const std::wstring& name) override {
    ++read_calls;
    if (fail_read_name == name) {
      return Result<std::optional<FirmwareVariableValue>>::failure(
          test_error(L"firmware read"));
    }
    const auto found = variables.find(name);
    return Result<std::optional<FirmwareVariableValue>>::success(
        found == variables.end()
            ? std::nullopt
            : std::optional<FirmwareVariableValue>(found->second));
  }

  Status replace_efi_global_variable_if_exact(
      const std::wstring& name,
      const std::optional<FirmwareVariableValue>& expected,
      const std::optional<FirmwareVariableValue>& replacement) override {
    ++write_calls;
    written_names.push_back(name);
    if (fail_write_name == name) {
      return Status::failure(test_error(L"firmware write"));
    }
    const auto found = variables.find(name);
    const std::optional<FirmwareVariableValue> current =
        found == variables.end()
        ? std::nullopt
        : std::optional<FirmwareVariableValue>(found->second);
    const auto same = [](const std::optional<FirmwareVariableValue>& left,
                         const std::optional<FirmwareVariableValue>& right) {
      return left.has_value() == right.has_value() &&
          (!left.has_value() ||
           (left->attributes == right->attributes && left->bytes == right->bytes));
    };
    if (!same(current, expected)) {
      return Status::failure(Error{
          .code = ErrorCode::identity_mismatch,
          .native_code = ERROR_DEVICE_NOT_CONNECTED,
          .operation = L"firmware conditional replace",
          .message = L"synthetic expected-value mismatch",
      });
    }
    if (replacement.has_value()) {
      variables[name] = replacement.value();
    } else {
      variables.erase(name);
      ++delete_calls;
      deleted_names.push_back(name);
    }
    if (corrupt_boot_order_after_write && name == L"BootOrder" &&
        replacement.has_value() && !variables[name].bytes.empty()) {
      variables[name].bytes[0] ^= std::byte{0x7F};
      corrupt_boot_order_after_write = false;
    }
    return ytec::clonecore::success_status();
  }

  std::map<std::wstring, FirmwareVariableValue> variables;
  std::wstring fail_read_name;
  std::wstring fail_write_name;
  std::size_t fail_revalidate_at{};
  bool corrupt_boot_order_after_write{};
  std::size_t revalidate_calls{};
  std::size_t read_calls{};
  std::size_t write_calls{};
  std::size_t delete_calls{};
  std::vector<std::wstring> written_names;
  std::vector<std::wstring> deleted_names;
};

FirmwareVariableValue option_for(
    const ytec::diskmodel::PartitionInfo& partition,
    const bool active = true) {
  auto result = ytec::bootrepair::build_windows_boot_manager_load_option(
      partition, 512U);
  check(result.has_value(), "synthetic load option should build");
  FirmwareVariableValue value{
      .bytes = result ? result.take_value() : std::vector<std::byte>{},
      .attributes = 7U,
  };
  if (!active && value.bytes.size() >= 4U) {
    value.bytes[0] = std::byte{0x00};
  }
  return value;
}

void test_load_option_round_trip_and_tamper() {
  const auto expected = esp();
  const auto built = ytec::bootrepair::build_windows_boot_manager_load_option(
      expected, 512U);
  check(built.has_value(), "exact GPT ESP load option should build");
  if (!built) {
    return;
  }
  const auto matches =
      ytec::bootrepair::windows_boot_manager_load_option_matches(
          built.value(), expected, 512U);
  check(matches.has_value() && matches.value(),
        "built load option should match the exact ESP");

  auto other = expected;
  other.identifier = L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";
  const auto wrong_guid =
      ytec::bootrepair::windows_boot_manager_load_option_matches(
          built.value(), other, 512U);
  check(wrong_guid.has_value() && !wrong_guid.value(),
        "another GPT partition GUID must not match");

  auto tampered = built.value();
  tampered[0] ^= std::byte{0x80};
  const auto attribute_only =
      ytec::bootrepair::windows_boot_manager_load_option_matches(
          tampered, expected, 512U);
  check(attribute_only.has_value() && attribute_only.value(),
        "load-option active attributes must not change its target identity");
  tampered = built.value();
  tampered[72] ^= std::byte{0x01};
  const auto signature_tamper =
      ytec::bootrepair::windows_boot_manager_load_option_matches(
          tampered, expected, 512U);
  check(signature_tamper.has_value() && !signature_tamper.value(),
        "partition-signature tampering must not match");
}

void test_confirmation_stops_before_platform() {
  auto request = valid_request();
  request.confirmation.typed_token = L"ok";
  MockPlatform platform;
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "lowercase token must fail closed");
  check(platform.read_calls == 0U && platform.write_calls == 0U &&
            platform.revalidate_calls == 0U,
        "confirmation failure must make zero platform calls");
}

void test_existing_active_entry_is_read_only_success() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.variables[L"BootOrder"] = boot_order({5U});
  platform.variables[L"Boot0005"] = option_for(request.expected_esp);
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(result.has_value(), "existing exact active entry should succeed");
  check(result.has_value() && result.value().already_valid &&
            result.value().prior_boot_order_preserved &&
            result.value().exact_target_verified,
        "existing exact entry must report verified no-op");
  check(platform.write_calls == 0U && platform.delete_calls == 0U,
        "existing exact entry must not mutate firmware");
}

void test_new_entry_appends_without_reordering() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.variables[L"BootOrder"] = boot_order({1U, 7U});
  platform.variables[L"Boot0001"] = option_for(
      esp(2U, L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"));
  platform.variables[L"Boot0007"] = option_for(
      esp(3U, L"{BBBBBBBB-CCCC-DDDD-EEEE-FFFFFFFFFFFF}"));
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(result.has_value(), "missing exact entry should be created");
  check(result.has_value() && result.value().boot_option_number == 0U &&
            result.value().boot_option_written &&
            result.value().boot_order_written &&
            result.value().prior_boot_order_preserved &&
            result.value().exact_target_verified,
        "new entry must be verified and appended to the old order");
  check(platform.variables[L"BootOrder"].bytes == boot_order({1U, 7U, 0U}).bytes,
        "existing BootOrder prefix must remain byte-for-byte unchanged");
}

void test_inactive_entry_only_activates_same_option() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.variables[L"BootOrder"] = boot_order({9U});
  platform.variables[L"Boot0009"] = option_for(request.expected_esp, false);
  const auto old_order = platform.variables[L"BootOrder"];
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(result.has_value(), "inactive exact option should be repaired");
  check(result.has_value() && result.value().boot_option_written &&
            !result.value().boot_order_written,
        "inactive option should not rewrite BootOrder");
  check(platform.variables[L"BootOrder"].bytes == old_order.bytes,
        "BootOrder must remain unchanged when the entry is already listed");
}

void test_duplicate_exact_entries_fail_before_write() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.variables[L"BootOrder"] = boot_order({2U, 3U});
  platform.variables[L"Boot0002"] = option_for(request.expected_esp);
  platform.variables[L"Boot0003"] = option_for(request.expected_esp);
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "duplicate exact target entries must fail closed");
  check(platform.write_calls == 0U && platform.delete_calls == 0U,
        "duplicate detection must occur before firmware mutation");
}

void test_target_drift_before_order_write_rolls_back_option() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.fail_revalidate_at = 3U;
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "target drift before BootOrder write must fail");
  check(platform.variables.find(L"Boot0000") == platform.variables.end(),
        "new boot option must be deleted when target revalidation fails");
  check(platform.delete_calls == 1U,
        "rollback must use the exact created Boot#### name once");
}

void test_boot_order_readback_mismatch_does_not_overwrite_unknown_value() {
  const auto request = valid_request();
  MockPlatform platform;
  const auto original = boot_order({4U});
  platform.variables[L"BootOrder"] = original;
  platform.variables[L"Boot0004"] = option_for(
      esp(4U, L"{CCCCCCCC-DDDD-EEEE-FFFF-AAAAAAAAAAAA}"));
  platform.corrupt_boot_order_after_write = true;
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "corrupt BootOrder readback must fail");
  check(platform.variables.find(L"Boot0000") == platform.variables.end(),
        "created Boot#### may be conditionally removed after an order mismatch");
  check(platform.variables[L"BootOrder"].bytes != original.bytes,
        "rollback must not overwrite an unexpected concurrent order value");
}

void test_hidden_matching_option_is_used_before_first_absent_slot() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.variables[L"BootOrder"] = boot_order({7U});
  platform.variables[L"Boot0007"] = option_for(
      esp(7U, L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"));
  platform.variables[L"Boot0002"] = option_for(request.expected_esp);
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(result.has_value() && result.value().boot_option_number == 2U,
        "a matching option outside BootOrder must win over an earlier absent slot");
  check(platform.variables.find(L"Boot0000") == platform.variables.end(),
        "hidden exact option discovery must not create a duplicate option");
  check(platform.variables[L"BootOrder"].bytes == boot_order({7U, 2U}).bytes,
        "the discovered option must be appended without reordering the prefix");
}

void test_final_target_drift_rolls_back_exact_written_values() {
  const auto request = valid_request();
  MockPlatform platform;
  platform.fail_revalidate_at = 4U;
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "final target drift must fail the transaction");
  check(platform.variables.find(L"Boot0000") == platform.variables.end() &&
            platform.variables.find(L"BootOrder") == platform.variables.end(),
        "final target drift must conditionally restore both original absences");
}

void test_non_esp_type_is_pre_platform_failure() {
  auto request = valid_request();
  request.expected_esp.type =
      L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
  MockPlatform platform;
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "a basic-data partition must not be treated as ESP");
  check(platform.read_calls == 0U && platform.write_calls == 0U &&
            platform.revalidate_calls == 0U,
        "wrong partition type must stop before firmware or target access");
}

void test_unsupported_4kn_is_pre_platform_failure() {
  auto request = valid_request();
  request.logical_sector_size = 4096U;
  request.expected_disk.logical_sector_size = 4096U;
  MockPlatform platform;
  const auto result = ytec::bootrepair::execute_current_pc_nvram_repair(
      request, platform);
  check(!result.has_value(), "4Kn firmware path must remain fail closed");
  check(platform.read_calls == 0U && platform.write_calls == 0U,
        "unsupported geometry must stop before firmware access");
}

}  // namespace

int main() {
  test_load_option_round_trip_and_tamper();
  test_confirmation_stops_before_platform();
  test_existing_active_entry_is_read_only_success();
  test_new_entry_appends_without_reordering();
  test_inactive_entry_only_activates_same_option();
  test_duplicate_exact_entries_fail_before_write();
  test_target_drift_before_order_write_rolls_back_option();
  test_boot_order_readback_mismatch_does_not_overwrite_unknown_value();
  test_hidden_matching_option_is_used_before_first_absent_slot();
  test_final_target_drift_rolls_back_exact_written_values();
  test_non_esp_type_is_pre_platform_failure();
  test_unsupported_4kn_is_pre_platform_failure();
  if (failures != 0) {
    std::cerr << failures << " NVRAM repair test(s) failed\n";
    return 1;
  }
  std::cout << "PASS: current-PC UEFI NVRAM repair transaction tests\n";
  return 0;
}

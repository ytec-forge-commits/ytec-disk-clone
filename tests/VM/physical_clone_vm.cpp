#include "vm_clone_execution_service.h"

#include "ytec/clonecore/disk_identity.h"
#include "ytec/diskmodel/inventory_formatter.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

struct Arguments final {
  bool plan{};
  bool execute{};
  bool boot_test{};
  bool legacy_bios_test{};
  std::optional<std::uint32_t> source_number;
  std::optional<std::uint32_t> target_number;
  std::wstring confirmation;
  std::wstring authorization;
};

void print_error(const ytec::clonecore::Error& error) {
  std::cerr << "YDC_ERROR code=" << static_cast<unsigned int>(error.code)
            << " native=" << error.native_code
            << " operation=" << ytec::diskmodel::to_utf8(error.operation)
            << " message=" << ytec::diskmodel::to_utf8(error.message) << '\n';
}

std::optional<std::uint32_t> parse_disk_number(const std::wstring& text) {
  if (text.empty() || text.size() > 10) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    value = value * 10U + static_cast<std::uint64_t>(character - L'0');
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
      return std::nullopt;
    }
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<Arguments> parse_arguments(const int argc, wchar_t* argv[]) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--plan") {
      arguments.plan = true;
    } else if (argument == L"--execute") {
      arguments.execute = true;
    } else if (argument == L"--boot-test") {
      arguments.boot_test = true;
    } else if (argument == L"--legacy-bios-test") {
      arguments.legacy_bios_test = true;
    } else if (argument == L"--source" && index + 1 < argc) {
      arguments.source_number = parse_disk_number(argv[++index]);
      if (!arguments.source_number.has_value()) {
        return std::nullopt;
      }
    } else if (argument == L"--target" && index + 1 < argc) {
      arguments.target_number = parse_disk_number(argv[++index]);
      if (!arguments.target_number.has_value()) {
        return std::nullopt;
      }
    } else if (argument == L"--confirmation" && index + 1 < argc) {
      arguments.confirmation = argv[++index];
    } else if (argument == L"--authorization" && index + 1 < argc) {
      arguments.authorization = argv[++index];
    } else {
      return std::nullopt;
    }
  }
  if (arguments.plan == arguments.execute ||
      (arguments.boot_test && arguments.legacy_bios_test) ||
      !arguments.source_number.has_value() ||
      !arguments.target_number.has_value() ||
      arguments.source_number == arguments.target_number) {
    return std::nullopt;
  }
  return arguments;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  const auto arguments = parse_arguments(argc, argv);
  if (!arguments.has_value()) {
    std::wcerr << L"Usage: --plan|--execute --source N --target N "
                  L"[--boot-test|--legacy-bios-test] "
                  L"[--authorization TOKEN --confirmation TEXT]\n";
    return 64;
  }
  if (!ytec::vmtest::is_virtualbox_guest()) {
    std::wcerr << L"ERROR: VirtualBoxゲスト以外では実行できません\n";
    return 65;
  }

  const auto profile = arguments->legacy_bios_test
      ? ytec::vmtest::VmCloneProfile::legacy_bios
      : arguments->boot_test ? ytec::vmtest::VmCloneProfile::boot
                             : ytec::vmtest::VmCloneProfile::synthetic;
  const auto selection = ytec::vmtest::select_vm_clone_disks(
      arguments->source_number.value(),
      arguments->target_number.value(),
      profile);
  if (!selection) {
    print_error(selection.error());
    return 1;
  }
  const std::wstring token = ytec::clonecore::make_target_confirmation_token(
      selection.value().target_identity);
  if (arguments->plan) {
    std::wcout << L"YDC_VM_PLAN_V1\n"
               << L"profile="
               << (arguments->legacy_bios_test
                       ? L"legacy-bios"
                       : arguments->boot_test ? L"boot" : L"synthetic")
               << L'\n'
               << L"sourceDisk=" << selection.value().source_disk.disk_number
               << L" sourceBytes=" << selection.value().source_disk.size_bytes
               << L" sourceSerialSuffix="
               << std::wstring(
                      selection.value().source_disk.serial_suffix.begin(),
                      selection.value().source_disk.serial_suffix.end())
               << L"\ntargetDisk=" << selection.value().target_disk.disk_number
               << L" targetBytes=" << selection.value().target_disk.size_bytes
               << L" targetSerialSuffix="
               << std::wstring(
                      selection.value().target_disk.serial_suffix.begin(),
                      selection.value().target_disk.serial_suffix.end())
               << L"\nconfirmation=" << token << L'\n';
    return 0;
  }

  ytec::vmtest::VmCloneExecutionService service;
  const auto result = service.execute(ytec::winpeapp::CloneExecutionRequest{
      .expected_source = selection.value().source_identity,
      .expected_target = selection.value().target_identity,
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = arguments->confirmation,
      },
      .authorization = arguments->authorization,
  });
  if (!result) {
    print_error(result.error());
    std::cerr << "TARGET_STATE=UNKNOWN_OR_OFFLINE\n";
    return 1;
  }

  std::wcout << L"YDC_VM_CLONE_PASS\n"
             << L"copiedDataBytes=" << result.value().copied_data_bytes << L'\n'
             << L"copiedPartitions="
             << result.value().copied_partition_count << L'\n'
             << L"recreatedPartitions="
             << result.value().recreated_partition_count << L'\n'
             << L"readBackVerified="
             << (result.value().read_back_verified ? L"true" : L"false")
             << L'\n'
             << L"partitionStyle="
             << (result.value().partition_style ==
                         ytec::winpeapp::ClonePartitionStyle::gpt
                     ? L"GPT"
                     : L"MBR")
             << L'\n'
             << L"partitionTableCommitted="
             << (result.value().partition_table_committed ? L"true" : L"false")
             << L'\n';
  return 0;
}

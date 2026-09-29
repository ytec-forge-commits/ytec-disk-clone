#include "ytec/bootrepair/efi_delete_transaction_windows.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* const message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

ytec::bootrepair::BcdBootRequest request(
    std::wstring windows,
    const ytec::bootrepair::BcdBootStorePolicy policy) {
  return ytec::bootrepair::BcdBootRequest{
      .target_windows_directory = std::move(windows),
      .target_system_partition_root = L"S:\\",
      .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
      .store_policy = policy,
  };
}

ytec::bootrepair::BcdStoreFileIdentity bcd_identity(
    const unsigned char seed) {
  ytec::bootrepair::BcdStoreFileIdentity identity{
      .volume_serial_number = 0x11223344ULL,
      .length = 4096U,
      .last_write_time = 55U,
      .change_time = 66U,
  };
  identity.file_id.fill(std::byte{seed});
  return identity;
}

void failed_bcd_backup_move_readback_is_fail_closed() {
  const auto expected = bcd_identity(0x41U);
  const auto foreign = bcd_identity(0x42U);
  const auto unchanged = ytec::bootrepair::
      classify_windows_efi_delete_failed_bcd_backup_move_readback(
          expected, expected, std::nullopt);
  check(unchanged == ytec::bootrepair::EfiDeleteMutationExtent::none,
        "only exact source plus absent destination proves no BCD mutation");

  const auto moved = ytec::bootrepair::
      classify_windows_efi_delete_failed_bcd_backup_move_readback(
          expected, std::nullopt, expected);
  const auto destination_race = ytec::bootrepair::
      classify_windows_efi_delete_failed_bcd_backup_move_readback(
          expected, expected, foreign);
  const auto source_drift = ytec::bootrepair::
      classify_windows_efi_delete_failed_bcd_backup_move_readback(
          expected, foreign, std::nullopt);
  check(moved == ytec::bootrepair::EfiDeleteMutationExtent::partial_or_unknown &&
            destination_race ==
                ytec::bootrepair::EfiDeleteMutationExtent::partial_or_unknown &&
            source_drift ==
                ytec::bootrepair::EfiDeleteMutationExtent::partial_or_unknown,
        "moved, foreign-destination, and drifted-source readbacks stay partial");
}

void failure_extent_is_honest() {
  const auto before = ytec::bootrepair::
      make_windows_efi_delete_injected_failure(
          ytec::bootrepair::WindowsEfiDeleteFailurePoint::
              before_candidate_delete,
          false);
  check(!before.succeeded && before.mutation_extent ==
            ytec::bootrepair::EfiDeleteMutationExtent::none,
        "a before-injection must report proven no mutation");

  const auto after = ytec::bootrepair::
      make_windows_efi_delete_injected_failure(
          ytec::bootrepair::WindowsEfiDeleteFailurePoint::
              after_candidate_delete,
          true);
  check(!after.succeeded && after.mutation_extent ==
            ytec::bootrepair::EfiDeleteMutationExtent::partial_or_unknown,
        "an after-injection must never claim no mutation");

  const auto bcd_backup_moved = ytec::bootrepair::
      make_windows_efi_delete_injected_failure(
          ytec::bootrepair::WindowsEfiDeleteFailurePoint::
              after_bcd_backup_move,
          true);
  check(!bcd_backup_moved.succeeded &&
            bcd_backup_moved.mutation_extent ==
                ytec::bootrepair::EfiDeleteMutationExtent::partial_or_unknown,
        "failure injected after the outer BCD move must remain partial");

  const auto none = ytec::bootrepair::
      make_windows_efi_delete_injected_failure(
          ytec::bootrepair::WindowsEfiDeleteFailurePoint::none,
          false);
  check(none.failure_kind == ytec::bootrepair::
            EfiDeletePlatformFailureKind::platform_contract_violation,
        "none is not a valid failure point");
}

void factory_rejects_unsafe_bcd_batches_without_io() {
  auto empty = ytec::bootrepair::
      make_windows_efi_delete_transaction_platform_for_failure_injection(
          {}, {});
  check(!empty.has_value(), "an empty BCD batch must fail closed");

  auto bios = request(
      L"D:\\Windows",
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh);
  bios.firmware = ytec::bootrepair::BcdBootFirmware::bios;
  auto wrong_firmware = ytec::bootrepair::
      make_windows_efi_delete_transaction_platform_for_failure_injection(
          {bios}, {});
  check(!wrong_firmware.has_value(),
        "the EFI delete adapter must reject BIOS requests");

  auto mismatched = ytec::bootrepair::
      make_windows_efi_delete_transaction_platform_for_failure_injection(
          {
              request(
                  L"D:\\Windows",
                  ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh),
              request(
                  L"E:\\Windows",
                  ytec::bootrepair::BcdBootStorePolicy::preserve_existing),
          },
          {ytec::bootrepair::WindowsEfiDeleteFailurePoint::
               before_quarantine_create,
           std::nullopt});
  check(mismatched.has_value(),
        "a reviewed UEFI rebuild/preserve batch should construct without IO");
  check(mismatched.value()->verified_bcd_report() == std::nullopt,
        "no BCD report exists before a successful transaction");

  auto wrong_root_second = request(
      L"E:\\Windows",
      ytec::bootrepair::BcdBootStorePolicy::preserve_existing);
  wrong_root_second.target_system_partition_root = L"T:\\";
  auto mixed_roots = ytec::bootrepair::
      make_windows_efi_delete_transaction_platform_for_failure_injection(
          {
              request(
                  L"D:\\Windows",
                  ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh),
              wrong_root_second,
          },
          {});
  check(!mixed_roots.has_value(),
        "all BCDBoot requests must target one exact ESP root");

  auto wrong_policy = ytec::bootrepair::
      make_windows_efi_delete_transaction_platform_for_failure_injection(
          {request(
              L"D:\\Windows",
              ytec::bootrepair::BcdBootStorePolicy::preserve_existing)},
          {});
  check(!wrong_policy.has_value(),
        "the first request must rebuild and cannot preserve");
}

}  // namespace

int main() {
  failed_bcd_backup_move_readback_is_fail_closed();
  failure_extent_is_honest();
  factory_rejects_unsafe_bcd_batches_without_io();
  if (failures == 0) {
    std::cout << "efi delete transaction windows tests: PASS\n";
    return EXIT_SUCCESS;
  }
  std::cerr << "efi delete transaction windows tests: " << failures
            << " failure(s)\n";
  return EXIT_FAILURE;
}

#include "ytec/imageformat/windows_tsumugi_destination.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kRequiredBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kOwnedPartialBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::StableDiskIdentity disk_identity(
    const std::uint32_t number,
    std::wstring instance,
    std::string serial) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = number,
      .model = L"MOCK DISK " + std::to_wstring(number),
      .size_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = std::move(serial),
      .device_instance_id = std::move(instance),
      .is_system_disk = false,
  };
}

ytec::imageformat::WindowsImagePathIdentity path_identity(
    const std::byte seed,
    const std::uint64_t size = 0U) {
  ytec::imageformat::WindowsImagePathIdentity value{
      .volume_serial_number = 0x12345678ULL,
      .file_size = size,
  };
  value.file_id.fill(seed);
  return value;
}

ytec::imageformat::WindowsTsumugiDestinationGuardRequest request_for(
    const ytec::imageformat::WindowsTsumugiDestinationGuardPhase phase) {
  return ytec::imageformat::WindowsTsumugiDestinationGuardRequest{
      .final_path = L"D:\\backup\\system.tsumugi",
      .expected_source_disk =
          disk_identity(1U, L"MOCK\\SOURCE\\ONE", "SOURCE01"),
      .required_available_bytes = kRequiredBytes,
      .replace_existing = false,
      .phase = phase,
      .expected_owned_partial_bytes =
          phase == ytec::imageformat::
                       WindowsTsumugiDestinationGuardPhase::
                           before_commit_owned_partial
              ? kOwnedPartialBytes
              : 0U,
  };
}

ytec::imageformat::WindowsImageDestinationObservation observation() {
  const auto source =
      disk_identity(1U, L"MOCK\\SOURCE\\ONE", "SOURCE01");
  const auto destination =
      disk_identity(2U, L"MOCK\\DESTINATION\\TWO", "DEST0002");
  return ytec::imageformat::WindowsImageDestinationObservation{
      .canonical_final_path = L"D:\\backup\\system.tsumugi",
      .partial_path = L"D:\\backup\\system.tsumugi.partial",
      .destination_disk = destination,
      .connected_disks = {source, destination},
      .available_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL,
      .physical_disk_extent_count = 1U,
      .file_system =
          ytec::imageformat::WindowsImageDestinationFileSystem::ntfs,
      .parent_is_reparse = false,
      .final_exists = false,
      .partial_exists = false,
      .parent_identity = path_identity(std::byte{0x11}),
      .final_identity = std::nullopt,
      .partial_identity = std::nullopt,
  };
}

void test_before_stage_accepts_ntfs_and_exfat() {
  const auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  auto value = observation();
  check(
      ytec::imageformat::validate_windows_tsumugi_destination_observation(
          request, value)
          .has_value(),
      "A distinct single-disk NTFS destination should pass");
  value.file_system =
      ytec::imageformat::WindowsImageDestinationFileSystem::exfat;
  check(
      ytec::imageformat::validate_windows_tsumugi_destination_observation(
          request, value)
          .has_value(),
      "A distinct single-disk exFAT destination should pass");
}

void test_before_stage_rejects_unknown_partial_and_capacity_failure() {
  const auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  auto value = observation();
  value.partial_exists = true;
  value.partial_identity =
      path_identity(std::byte{0x22}, kOwnedPartialBytes);
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "An unknown pre-existing partial must fail before staging");

  value = observation();
  value.available_bytes = kRequiredBytes - 1U;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "Insufficient free space must fail before staging");
}

void test_commit_requires_exact_owned_partial_observation() {
  const auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::
          before_commit_owned_partial);
  auto value = observation();
  value.partial_exists = true;
  value.partial_identity =
      path_identity(std::byte{0x22}, kOwnedPartialBytes);
  value.available_bytes = 0U;
  check(
      ytec::imageformat::validate_windows_tsumugi_destination_observation(
          request, value)
          .has_value(),
      "Commit guard should not reserve bytes already held by owned partial");

  value.partial_identity->file_size = kOwnedPartialBytes - 1U;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A changed partial length must fail before commit");
  value.partial_identity.reset();
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A missing partial identity must fail before commit");
}

void test_rejects_reparse_multi_disk_filesystem_and_missing_parent_id() {
  const auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  auto value = observation();
  value.parent_is_reparse = true;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "Any reparse ancestor must fail");

  value = observation();
  value.physical_disk_extent_count = 2U;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A multi-disk destination must fail");

  value = observation();
  value.file_system =
      ytec::imageformat::WindowsImageDestinationFileSystem::other;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A non-NTFS/exFAT destination must fail");

  value = observation();
  value.parent_identity.reset();
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A parent without handle identity evidence must fail");
}

void test_source_alias_and_unstable_source_observation_fail() {
  const auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  auto value = observation();
  value.destination_disk = request.expected_source_disk;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "The source disk cannot back the image destination");

  value = observation();
  value.connected_disks.erase(value.connected_disks.begin());
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A disappeared source must fail reidentification");
  value = observation();
  value.connected_disks.push_back(request.expected_source_disk);
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "An ambiguous source identity must fail reidentification");
}

void test_replacement_requires_confirmation_and_regular_identity() {
  auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  auto value = observation();
  value.final_exists = true;
  value.final_identity = path_identity(std::byte{0x33}, 4096U);
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "Existing final image replacement must require confirmation");
  request.replace_existing = true;
  check(
      ytec::imageformat::validate_windows_tsumugi_destination_observation(
          request, value)
          .has_value(),
      "Confirmed replacement with identity evidence should pass");
  value.final_identity.reset();
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "Existing final without identity evidence must fail");
}

void test_invalid_requests_and_path_drift_fail_closed() {
  auto request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  auto value = observation();
  request.final_path = L"backup\\system.tsumugi";
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A relative path must fail before filesystem use");

  request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  request.expected_owned_partial_bytes = 1U;
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "Pre-stage must not claim a partial");

  request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::before_stage);
  value.canonical_final_path = L"D:\\backup\\other.tsumugi";
  value.partial_path = value.canonical_final_path + L".partial";
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "A canonical path drift must fail");

  request = request_for(
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase::
          before_commit_owned_partial);
  request.required_available_bytes = 0U;
  value = observation();
  value.partial_exists = true;
  value.partial_identity =
      path_identity(std::byte{0x22}, kOwnedPartialBytes);
  check(
      !ytec::imageformat::validate_windows_tsumugi_destination_observation(
           request, value)
           .has_value(),
      "Commit must retain a nonzero pre-stage capacity estimate");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"before_stage_accepts_ntfs_and_exfat",
       test_before_stage_accepts_ntfs_and_exfat},
      {"before_stage_rejects_unknown_partial_and_capacity_failure",
       test_before_stage_rejects_unknown_partial_and_capacity_failure},
      {"commit_requires_exact_owned_partial_observation",
       test_commit_requires_exact_owned_partial_observation},
      {"rejects_reparse_multi_disk_filesystem_and_missing_parent_id",
       test_rejects_reparse_multi_disk_filesystem_and_missing_parent_id},
      {"source_alias_and_unstable_source_observation_fail",
       test_source_alias_and_unstable_source_observation_fail},
      {"replacement_requires_confirmation_and_regular_identity",
       test_replacement_requires_confirmation_and_regular_identity},
      {"invalid_requests_and_path_drift_fail_closed",
       test_invalid_requests_and_path_drift_fail_closed},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  std::cout << (tests.size() - failures) << "/" << tests.size()
            << " tests passed\n";
  return failures == 0 ? 0 : 1;
}

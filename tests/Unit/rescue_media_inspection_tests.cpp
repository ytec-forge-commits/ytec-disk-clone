#include "rescue_media_test_fixture.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void strict_manifest_parser_accepts_only_schema_v2_contract() {
  const auto disk = rescue_media_test_fixture::owned_usb();
  const auto fixture = rescue_media_test_fixture::owned_manifest(disk);
  const std::string json = rescue_media_test_fixture::manifest_json(fixture);
  const auto parsed =
      ytec::windowsapp::parse_rescue_usb_ownership_manifest(
          rescue_media_test_fixture::bytes(json));
  check(parsed.has_value(), "Valid strict schema-v2 manifest should parse");
  check(
      parsed.value().canonical_layout == fixture.canonical_layout &&
          parsed.value().owned_boot_tree_identity ==
              fixture.owned_boot_tree_identity,
      "Parsed layout and tree identity must exactly match the fixture");

  std::string old_schema = json;
  const auto schema = old_schema.find("\"schemaVersion\":2");
  check(schema != std::string::npos, "Schema field should exist");
  old_schema.replace(schema, std::string("\"schemaVersion\":2").size(),
                     "\"schemaVersion\":1");
  check(
      !ytec::windowsapp::parse_rescue_usb_ownership_manifest(
          rescue_media_test_fixture::bytes(old_schema)),
      "Unknown ownership schema must fail closed");

  std::string duplicate = json;
  duplicate.insert(1U, "\"schemaVersion\":2,");
  check(
      !ytec::windowsapp::parse_rescue_usb_ownership_manifest(
          rescue_media_test_fixture::bytes(duplicate)),
      "Duplicate JSON field must fail closed");

  std::string lowercase_digest = json;
  const auto digest = lowercase_digest.find("\"rootDigest\":\"");
  check(digest != std::string::npos, "Digest field should exist");
  const std::size_t digest_value = digest +
      std::string("\"rootDigest\":\"").size();
  lowercase_digest[digest_value] = 'a';
  check(
      !ytec::windowsapp::parse_rescue_usb_ownership_manifest(
          rescue_media_test_fixture::bytes(lowercase_digest)),
      "Non-canonical lowercase SHA-256 must fail closed");

  auto invalid_utf8 = rescue_media_test_fixture::bytes(json);
  invalid_utf8.insert(invalid_utf8.end() - 1U, std::byte{0xFFU});
  check(
      !ytec::windowsapp::parse_rescue_usb_ownership_manifest(invalid_utf8),
      "Invalid UTF-8 must fail closed");
}

void owned_evidence_binds_identity_layout_mode_and_file_system() {
  const auto disk = rescue_media_test_fixture::owned_usb();
  const auto manifest = rescue_media_test_fixture::owned_manifest(disk);
  auto evidence = ytec::windowsapp::build_rescue_usb_owned_media_evidence(
      disk,
      manifest,
      rescue_media_test_fixture::marker_bytes(),
      rescue_media_test_fixture::boot_files(),
      rescue_media_test_fixture::boot_directories(),
      rescue_media_test_fixture::data_identity());
  check(evidence.has_value(), "Verified owned evidence should be buildable");
  check(
      !evidence.value().physical_write_started &&
          evidence.value().cache_key.mode ==
              ytec::windowsapp::RescueUsbProvisioningMode::
                  preserve_data_refresh &&
          evidence.value().cache_key.canonical_layout ==
              ytec::windowsapp::make_rescue_usb_canonical_layout(disk),
      "Evidence must bind complete read-only review state");
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_rescue_usb_inspection_evidence(
              evidence.value(), disk,
              ytec::windowsapp::RescueUsbProvisioningMode::
                  preserve_data_refresh,
              ytec::windowsapp::RescueUsbDataFileSystem::ntfs)),
      "Fresh exact evidence should validate");
  check(
      !ytec::windowsapp::validate_rescue_usb_inspection_evidence(
          evidence.value(), disk,
          ytec::windowsapp::RescueUsbProvisioningMode::initialize_all,
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs),
      "Inspection cache must not authorize another mode");

  auto drifted = disk;
  drifted.partitions[1].size_bytes -= 512U;
  check(
      !ytec::windowsapp::validate_rescue_usb_inspection_target_binding(
          evidence.value(), drifted),
      "Canonical full-layout drift must invalidate inspection evidence");

  auto wrong_marker = rescue_media_test_fixture::marker_bytes();
  wrong_marker.front() =
      static_cast<std::byte>(static_cast<unsigned char>('9'));
  check(
      !ytec::windowsapp::build_rescue_usb_owned_media_evidence(
          disk, manifest, wrong_marker,
          rescue_media_test_fixture::boot_files(),
          rescue_media_test_fixture::boot_directories(),
          rescue_media_test_fixture::data_identity()),
      "Marker and manifest media ID mismatch must fail closed");

  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = ytec::windowsapp::
          RescueUsbProvisioningMode::preserve_data_refresh,
      .data_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
      .owned_media = &evidence.value().owned_media,
  });
  check(plan.has_value(), "Reviewed refresh plan should be buildable");
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_rescue_usb_fresh_inspection_for_plan(
              evidence.value(), plan.value())),
      "An exact fresh inspection must match the reviewed plan");
  auto data_drift = evidence.value();
  ++data_drift.owned_media.observed_data_tree_identity.total_logical_bytes;
  check(
      !ytec::windowsapp::validate_rescue_usb_fresh_inspection_for_plan(
          data_drift, plan.value()),
      "Private data aggregate drift must invalidate the reviewed plan");
  auto boot_drift = evidence.value();
  ++boot_drift.owned_media.observed_boot_tree_identity.entry_count;
  check(
      !ytec::windowsapp::validate_rescue_usb_fresh_inspection_for_plan(
          boot_drift, plan.value()),
      "Owned boot-tree drift must invalidate the reviewed plan");
}

void boot_and_data_volume_roots_require_unique_exact_extents() {
  const auto disk = rescue_media_test_fixture::owned_usb();
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'R',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[0].offset_bytes,
              .length = disk.partitions[0].size_bytes,
          }},
      },
      {
          .drive_letter = L'S',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[1].offset_bytes,
              .length = disk.partitions[1].size_bytes,
          }},
      },
  };
  const auto roots =
      ytec::windowsapp::resolve_rescue_usb_owned_volume_roots(
          disk, volumes, L"fat32", L"NTFS");
  check(roots.has_value(), "Exact boot and data extents should resolve");
  check(
      roots.value().boot_root == L"R:\\" &&
          roots.value().data_root == L"S:\\" &&
          !roots.value().physical_write_started,
      "Resolved roots must remain read-only and canonical");

  auto spanned = volumes;
  spanned[0].extents.push_back({
      .disk_number = 99U,
      .starting_offset = 0U,
      .length = 4096U,
  });
  check(
      !ytec::windowsapp::resolve_rescue_usb_owned_volume_roots(
          disk, spanned, L"FAT32", L"NTFS"),
      "Spanned boot extent must fail closed");

  auto ambiguous = volumes;
  ambiguous.push_back(volumes[1]);
  ambiguous.back().drive_letter = L'T';
  check(
      !ytec::windowsapp::resolve_rescue_usb_owned_volume_roots(
          disk, ambiguous, L"FAT32", L"NTFS"),
      "Ambiguous data drive letter must fail closed");
  check(
      !ytec::windowsapp::resolve_rescue_usb_owned_volume_roots(
          disk, volumes, L"FAT32", L"ReFS"),
      "Unexpected data filesystem must fail closed");
}

void cancelled_product_inspection_stops_before_windows_io() {
  const auto disk = rescue_media_test_fixture::owned_usb();
  std::atomic_bool cancelled{true};
  const auto result =
      ytec::windowsapp::inspect_rescue_usb_owned_media_with_windows_apis(
          disk, &cancelled);
  check(
      result.state ==
              ytec::windowsapp::RescueUsbInspectionState::blocked &&
          !result.physical_write_started &&
          result.message.find(L"取り消") != std::wstring::npos,
      "Cancelled product inspection must stop before any Windows volume access");

  const auto evidence =
      ytec::windowsapp::build_rescue_usb_owned_media_evidence(
          disk,
          rescue_media_test_fixture::owned_manifest(disk),
          rescue_media_test_fixture::marker_bytes(),
          rescue_media_test_fixture::boot_files(),
          rescue_media_test_fixture::boot_directories(),
          rescue_media_test_fixture::data_identity());
  check(evidence.has_value(), "Owned evidence fixture should succeed");
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = ytec::windowsapp::
          RescueUsbProvisioningMode::preserve_data_refresh,
      .data_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
      .owned_media = &evidence.value().owned_media,
  });
  check(plan.has_value(), "Reviewed refresh plan should be buildable");
  check(
      !ytec::windowsapp::
          reinspect_rescue_usb_storage_plan_with_windows_apis(
              plan.value(), &cancelled),
      "Cancelled final reinspection must stop before disk enumeration");
}

}  // namespace

int main() {
  try {
    strict_manifest_parser_accepts_only_schema_v2_contract();
    owned_evidence_binds_identity_layout_mode_and_file_system();
    boot_and_data_volume_roots_require_unique_exact_extents();
    cancelled_product_inspection_stops_before_windows_io();
    std::cout << "rescue media inspection tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rescue media inspection test failed: "
              << error.what() << '\n';
    return 1;
  }
}

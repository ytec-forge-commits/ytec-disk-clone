#include "ytec/windowsapp/windows_adk_eula_extractor.h"
#include "ytec/windowsapp/windows_adk_acquisition_platform.h"

#include <Windows.h>
#include <Objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

void write_u16(
    std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(
    std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

std::array<std::byte, 36U> valid_cab_header() {
  std::array<std::byte, 36U> header{};
  header[0] = std::byte{'M'};
  header[1] = std::byte{'S'};
  header[2] = std::byte{'C'};
  header[3] = std::byte{'F'};
  write_u32(header, 8U, 4'096U);
  write_u32(header, 16U, 36U);
  header[24U] = std::byte{3U};
  header[25U] = std::byte{1U};
  write_u16(header, 26U, 1U);
  write_u16(header, 28U, 2U);
  write_u16(header, 32U, 7U);
  return header;
}

std::vector<std::byte> bytes_of(const std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

std::string hash_of(const char value) {
  return std::string(64U, value);
}

ytec::windowsapp::AdkEmbeddedEulaPin eula_pin() {
  return ytec::windowsapp::AdkEmbeddedEulaPin{
      .source_payload_kind =
          ytec::windowsapp::AdkPayloadKind::deployment_tools,
      .official_bootstrap_url =
          L"https://download.microsoft.com/download/a/adksetup.exe",
      .container_offset = 10U,
      .container_length = 80U,
      .container_member_name = L"u6",
      .display_file_name = L"ja\\eula.rtf",
      .expected_byte_count = 293'766U,
      .expected_sha256 = hash_of('D'),
      .expected_document_title =
          L"WINDOWS ASSESSMENT AND DEPLOYMENT KIT (ADK)",
      .primary_source_confirmed = true,
  };
}

ytec::windowsapp::WindowsAdkEulaExtractionObservation
complete_observation(const ytec::windowsapp::AdkEmbeddedEulaPin& pin) {
  return ytec::windowsapp::WindowsAdkEulaExtractionObservation{
      .bootstrap_full_identity_verified = true,
      .attached_container_created_new = true,
      .attached_container_regular_file = true,
      .attached_container_single_link = true,
      .attached_container_reparse_point = false,
      .attached_container_bounds_verified = true,
      .attached_container_readback_equal = true,
      .cabinet_api_structure_verified = true,
      .burn_manifest_bounded = true,
      .burn_ux_mapping_verified = true,
      .member_copy_created_new = true,
      .member_copy_regular_file = true,
      .member_copy_single_link = true,
      .member_copy_reparse_point = false,
      .member_byte_count = pin.expected_byte_count,
      .member_sha256 = pin.expected_sha256,
      .member_title_verified = true,
      .bounded_read_complete = true,
      .temporary_files_removed = true,
  };
}

void test_cab_header_is_bounded_and_single_cabinet_only() {
  const auto header = valid_cab_header();
  const auto parsed =
      ytec::windowsapp::inspect_windows_adk_embedded_cab_header(
          header, 4'096U);
  check(
      parsed && parsed.value().cabinet_byte_count == 4'096U &&
          parsed.value().file_count == 2U &&
          parsed.value().folder_count == 1U &&
          parsed.value().set_id == 7U,
      "A bounded single CAB header should pass");

  auto continued = header;
  write_u16(continued, 30U, 1U);
  check(
      !ytec::windowsapp::inspect_windows_adk_embedded_cab_header(
          continued, 4'096U),
      "Reserved or continuation CAB flags must fail closed");
  check(
      !ytec::windowsapp::inspect_windows_adk_embedded_cab_header(
          header, 4'095U),
      "The observed CAB length must exactly match the pin");
}

void test_burn_mapping_requires_one_exact_namespaced_payload() {
  const auto pin = eula_pin();
  const auto valid = bytes_of(
      "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">"
      "<Payload SourcePath=\"u6\" FilePath=\"ja\\eula.rtf\" "
      "FileSize=\"293766\"/>"
      "</BurnManifest>");
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_windows_adk_burn_eula_mapping(
              valid, pin)),
      "The exact Burn UX mapping should pass");

  const auto case_alias = bytes_of(
      "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">"
      "<Payload SourcePath=\"U6\" FilePath=\"ja\\eula.rtf\" "
      "FileSize=\"293766\"/>"
      "</BurnManifest>");
  check(
      !ytec::windowsapp::validate_windows_adk_burn_eula_mapping(
          case_alias, pin),
      "Case aliases must be rejected rather than guessed");

  const auto duplicate = bytes_of(
      "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">"
      "<Payload SourcePath=\"u6\" FilePath=\"ja\\eula.rtf\" "
      "FileSize=\"293766\"/><Payload SourcePath=\"u6\" "
      "FilePath=\"ja\\eula.rtf\" FileSize=\"293766\"/>"
      "</BurnManifest>");
  check(
      !ytec::windowsapp::validate_windows_adk_burn_eula_mapping(
          duplicate, pin),
      "Duplicate exact EULA mappings must fail");

  const auto dtd = bytes_of(
      "<!DOCTYPE BurnManifest [<!ENTITY x \"u6\">]>"
      "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">"
      "<Payload SourcePath=\"&x;\" FilePath=\"ja\\eula.rtf\" "
      "FileSize=\"293766\"/>"
      "</BurnManifest>");
  check(
      !ytec::windowsapp::validate_windows_adk_burn_eula_mapping(dtd, pin),
      "DTD expansion must be prohibited");
}

void test_receipt_requires_every_native_proof() {
  const auto pin = eula_pin();
  auto observation = complete_observation(pin);
  const auto receipt = ytec::windowsapp::finalize_windows_adk_eula_receipt(
      pin, observation);
  check(
      receipt && receipt.value().bootstrap_full_identity_verified &&
          receipt.value().temporary_files_removed,
      "All exact native facts should produce a bounded receipt");

  observation.member_copy_single_link = false;
  check(
      !ytec::windowsapp::finalize_windows_adk_eula_receipt(
          pin, observation),
      "A hard-linked member must fail finalization");
  observation = complete_observation(pin);
  observation.temporary_files_removed = false;
  check(
      !ytec::windowsapp::finalize_windows_adk_eula_receipt(
          pin, observation),
      "Unknown leftover temporary files must fail finalization");
}

void test_source_identity_binds_bootstrap_and_embedded_pin() {
  const auto pin = eula_pin();
  const ytec::windowsapp::AdkPinnedPayload pinned{
      .kind = ytec::windowsapp::AdkPayloadKind::deployment_tools,
      .installer_kind =
          ytec::windowsapp::AdkInstallerKind::microsoft_bootstrap_exe,
      .display_name = L"Windows ADK Deployment Tools",
      .staging_file_name = L"adksetup.exe",
      .offline_relative_path = L"adksetup.exe",
      .exact_source_url = pin.official_bootstrap_url,
      .expected_sha256 = hash_of('A'),
      .expected_signer_subject = L"Microsoft Corporation",
      .expected_payload_version = L"10.1.26100.2454",
      .expected_byte_count = 100U,
      .maximum_bytes = 1'024U,
  };
  auto verified = ytec::windowsapp::AdkVerifiedPayload{
      .kind = pinned.kind,
      .installer_kind = pinned.installer_kind,
      .staged_path = L"C:\\stage\\adksetup.exe",
      .byte_count = pinned.expected_byte_count,
      .sha256 = pinned.expected_sha256,
      .signer_subject = pinned.expected_signer_subject,
      .payload_version = pinned.expected_payload_version,
  };
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_windows_adk_eula_source_identity(
              pinned, verified, pin)),
      "The exact verified bootstrap and EULA pin should match");
  verified.sha256 = hash_of('B');
  check(
      !ytec::windowsapp::validate_windows_adk_eula_source_identity(
          pinned, verified, pin),
      "A changed bootstrap hash must fail before extraction");
}

bool run_official_bootstrap_probe(
    const std::filesystem::path& source_path) {
  using namespace ytec::windowsapp;
  const auto manifest = tsumugi_1_0_0_adk_manifest();
  const auto pinned = std::find_if(
      manifest.payloads.begin(),
      manifest.payloads.end(),
      [](const AdkPinnedPayload& payload) {
        return payload.kind == AdkPayloadKind::deployment_tools;
      });
  if (pinned == manifest.payloads.end()) {
    std::cerr << "official probe: deployment payload pin missing\n";
    return false;
  }
  WindowsAdkAcquisitionPlatform platform;
  const auto staging = platform.create_new_staging_area(
      pinned->maximum_bytes + manifest.embedded_eula.container_length);
  if (!staging) {
    std::wcerr << L"official probe staging failed: "
               << staging.error().operation << L": "
               << staging.error().message << L'\n';
    return false;
  }
  const auto cleanup = [&]() {
    const auto removed = platform.remove_staging_area(staging.value());
    if (!removed) {
      std::wcerr << L"official probe cleanup failed: "
                 << removed.error().operation << L": "
                 << removed.error().message << L'\n';
      return false;
    }
    return true;
  };
  const auto receipt = platform.stage_offline_payload(
      AdkOfflineStageRequest{
          .layout_root = source_path.parent_path(),
          .exact_relative_path = source_path.filename(),
          .create_new_destination =
              staging.value().root / pinned->staging_file_name,
          .maximum_bytes = pinned->maximum_bytes,
      });
  if (!receipt) {
    std::wcerr << L"official probe stage failed: "
               << receipt.error().operation << L": "
               << receipt.error().message << L'\n';
    static_cast<void>(cleanup());
    return false;
  }
  const auto hash = platform.sha256_file(
      receipt.value().staged_path, pinned->maximum_bytes);
  const auto signature = platform.verify_authenticode(
      receipt.value().staged_path, pinned->expected_signer_subject);
  const auto version = platform.query_payload_version(
      receipt.value().staged_path);
  if (!hash || !signature || !version) {
    const auto* error = !hash ? &hash.error()
                        : !signature ? &signature.error()
                                     : &version.error();
    std::wcerr << L"official probe identity failed: "
               << error->operation << L": " << error->message << L'\n';
    static_cast<void>(cleanup());
    return false;
  }
  const AdkVerifiedPayload verified{
      .kind = pinned->kind,
      .installer_kind = pinned->installer_kind,
      .staged_path = receipt.value().staged_path,
      .byte_count = receipt.value().byte_count,
      .sha256 = hash.value(),
      .signer_subject = pinned->expected_signer_subject,
      .payload_version = version.value(),
  };
  const auto extracted = platform.extract_verified_embedded_eula(
      *pinned, verified, manifest.embedded_eula);
  if (!extracted) {
    std::cerr << "official probe extraction native="
              << extracted.error().native_code << '\n';
    std::wcerr << L"official probe extraction failed: "
               << extracted.error().operation << L": "
               << extracted.error().message << L'\n';
    static_cast<void>(cleanup());
    return false;
  }
  const bool exact =
      extracted.value().rtf_document.size() ==
          manifest.embedded_eula.expected_byte_count &&
      extracted.value().receipt.extracted_identity ==
          manifest.embedded_eula;
  const bool cleaned = cleanup();
  if (!exact || !cleaned) {
    std::cerr << "official probe: receipt/body/cleanup mismatch\n";
    return false;
  }
  std::cout << "official ADK embedded EULA probe: PASS\n";
  return true;
}

}  // namespace

int main(const int argc, char* const argv[]) {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  try {
    test_cab_header_is_bounded_and_single_cabinet_only();
    test_burn_mapping_requires_one_exact_namespaced_payload();
    test_receipt_requires_every_native_proof();
    test_source_identity_binds_bootstrap_and_embedded_pin();
    if (argc == 2 &&
        !run_official_bootstrap_probe(std::filesystem::path(argv[1]))) {
      if (SUCCEEDED(initialized)) {
        CoUninitialize();
      }
      return 1;
    }
    if (SUCCEEDED(initialized)) {
      CoUninitialize();
    }
    std::cout << "windows ADK EULA extractor tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    if (SUCCEEDED(initialized)) {
      CoUninitialize();
    }
    std::cerr << "windows ADK EULA extractor tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}

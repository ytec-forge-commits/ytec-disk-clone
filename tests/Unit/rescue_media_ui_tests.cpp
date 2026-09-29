#include "rescue_media_test_fixture.h"

#include "ytec/windowsapp/rescue_media_ui.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Result<ytec::windowsapp::RescueUsbOwnedMediaEvidence>
evidence_for(const ytec::diskmodel::DiskInfo& disk) {
  return ytec::windowsapp::build_rescue_usb_owned_media_evidence(
      disk,
      rescue_media_test_fixture::owned_manifest(disk),
      rescue_media_test_fixture::marker_bytes(),
      rescue_media_test_fixture::boot_files(),
      rescue_media_test_fixture::boot_directories(),
      rescue_media_test_fixture::data_identity());
}

void non_usb_and_incomplete_inspection_never_enable_review() {
  const auto hidden = ytec::windowsapp::build_rescue_media_usb_ui_view({});
  check(!hidden.selectors_visible && !hidden.ready_for_review,
        "USB-only selectors must be hidden for ISO");

  const auto disk = rescue_media_test_fixture::initializable_usb();
  const auto scanning =
      ytec::windowsapp::build_rescue_media_usb_ui_view({
          .usb_kind_selected = true,
          .selected_target = &disk,
          .inspection_state =
              ytec::windowsapp::RescueUsbInspectionState::scanning,
          .selected_mode =
              ytec::windowsapp::RescueUsbProvisioningMode::initialize_all,
          .selected_file_system =
              ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
      });
  check(scanning.selectors_visible && !scanning.ready_for_review,
        "Scanning state must block review");

  const auto blocked =
      ytec::windowsapp::build_rescue_media_usb_ui_view({
          .usb_kind_selected = true,
          .selected_target = &disk,
          .inspection_state =
              ytec::windowsapp::RescueUsbInspectionState::blocked,
          .inspection_message = L"synthetic blocked",
      });
  check(!blocked.ready_for_review &&
            blocked.issue ==
                ytec::windowsapp::RescueMediaUsbUiIssue::inspection_blocked,
        "Failed inspection must block review");
}

void unknown_media_recommends_whole_disk_initialization() {
  const auto disk = rescue_media_test_fixture::initializable_usb();
  const auto view = ytec::windowsapp::build_rescue_media_usb_ui_view({
      .usb_kind_selected = true,
      .selected_target = &disk,
      .inspection_state =
          ytec::windowsapp::RescueUsbInspectionState::unknown_media,
      .selected_mode =
          ytec::windowsapp::RescueUsbProvisioningMode::initialize_all,
      .selected_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
  });
  check(view.ready_for_review && view.storage_plan.has_value(),
        "Unknown basic multi-partition USB should initialize");
  check(
      view.recommended_mode ==
              ytec::windowsapp::RescueUsbProvisioningMode::initialize_all &&
          view.recommended_file_system ==
              ytec::windowsapp::RescueUsbDataFileSystem::ntfs &&
          !view.mode_selector_enabled &&
          view.file_system_selector_enabled,
      "Unknown media must fix full initialization and default NTFS");
  check(
      view.confirmation_token == L"OK" &&
          view.review_summary.find(L"USBディスク全体") !=
              std::wstring::npos &&
          view.review_summary.find(L"正確に4GiB FAT32") !=
              std::wstring::npos,
      "Unknown media review must say full erase and exact split");

  const auto impossible_refresh =
      ytec::windowsapp::build_rescue_media_usb_ui_view({
          .usb_kind_selected = true,
          .selected_target = &disk,
          .inspection_state =
              ytec::windowsapp::RescueUsbInspectionState::unknown_media,
          .selected_mode = ytec::windowsapp::RescueUsbProvisioningMode::
              preserve_data_refresh,
          .selected_file_system =
              ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
      });
  check(!impossible_refresh.ready_for_review,
        "Unknown media must never authorize preserve refresh");
}

void verified_media_defaults_to_private_data_preserving_refresh() {
  const auto disk = rescue_media_test_fixture::owned_usb();
  auto evidence = evidence_for(disk);
  check(evidence.has_value(), "Owned evidence fixture should succeed");
  const auto view = ytec::windowsapp::build_rescue_media_usb_ui_view({
      .usb_kind_selected = true,
      .selected_target = &disk,
      .inspection_state =
          ytec::windowsapp::RescueUsbInspectionState::verified_owned,
      .owned_evidence = &evidence.value(),
      .selected_mode = ytec::windowsapp::RescueUsbProvisioningMode::
          preserve_data_refresh,
      .selected_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
  });
  check(view.ready_for_review && view.storage_plan.has_value(),
        "Verified owned media should offer refresh");
  check(view.mode_selector_enabled &&
            !view.file_system_selector_enabled &&
            view.recommended_mode ==
                ytec::windowsapp::RescueUsbProvisioningMode::
                    preserve_data_refresh,
        "Owned media must recommend refresh and lock existing filesystem");
  check(
      view.review_summary.find(L"非上書き切替") != std::wstring::npos &&
          view.review_summary.find(L"データ領域の全内容") !=
              std::wstring::npos &&
          view.review_summary.find(L"3件") != std::wstring::npos &&
          view.review_summary.find(L"11 bytes") != std::wstring::npos,
      "Refresh review must include only bounded count and byte evidence");
  check(
      view.review_summary.find(L"private-customer-name") ==
              std::wstring::npos &&
          view.review_summary.find(L"backup.tsumugi") ==
              std::wstring::npos &&
          view.review_summary.find(L"root") == std::wstring::npos,
      "Private paths and root digest must never appear in UI summary");

  const auto explicit_exfat_erase =
      ytec::windowsapp::build_rescue_media_usb_ui_view({
          .usb_kind_selected = true,
          .selected_target = &disk,
          .inspection_state =
              ytec::windowsapp::RescueUsbInspectionState::verified_owned,
          .owned_evidence = &evidence.value(),
          .selected_mode =
              ytec::windowsapp::RescueUsbProvisioningMode::initialize_all,
          .selected_file_system =
              ytec::windowsapp::RescueUsbDataFileSystem::exfat,
      });
  check(explicit_exfat_erase.ready_for_review &&
            explicit_exfat_erase.file_system_selector_enabled &&
            explicit_exfat_erase.review_summary.find(L"exFAT（明示選択）") !=
                std::wstring::npos,
        "Verified media may be explicitly erased with exFAT");
}

void full_layout_drift_invalidates_verified_media_view() {
  const auto disk = rescue_media_test_fixture::owned_usb();
  auto evidence = evidence_for(disk);
  check(evidence.has_value(), "Owned evidence fixture should succeed");
  auto drifted = disk;
  drifted.partitions[1].offset_bytes += 512U;
  const auto view = ytec::windowsapp::build_rescue_media_usb_ui_view({
      .usb_kind_selected = true,
      .selected_target = &drifted,
      .inspection_state =
          ytec::windowsapp::RescueUsbInspectionState::verified_owned,
      .owned_evidence = &evidence.value(),
      .selected_mode = ytec::windowsapp::RescueUsbProvisioningMode::
          preserve_data_refresh,
      .selected_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
  });
  check(!view.ready_for_review &&
            view.issue ==
                ytec::windowsapp::RescueMediaUsbUiIssue::inspection_drift,
        "Any canonical full-layout drift must block review");
}

}  // namespace

int main() {
  try {
    non_usb_and_incomplete_inspection_never_enable_review();
    unknown_media_recommends_whole_disk_initialization();
    verified_media_defaults_to_private_data_preserving_refresh();
    full_layout_drift_invalidates_verified_media_view();
    std::cout << "rescue media UI tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rescue media UI test failed: " << error.what() << '\n';
    return 1;
  }
}

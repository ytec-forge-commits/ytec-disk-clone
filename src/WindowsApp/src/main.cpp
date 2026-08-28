#include "ytec/clonecore/completion_power_action.h"
#include "ytec/clonecore/log.h"
#include "ytec/clonecore/manual_pause.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/operation_types.h"
#include "ytec/imageformat/tsumugi_crypto.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/imageformat/tsumugi_stream.h"
#include "ytec/uisupport/error_presentation.h"
#include "ytec/uisupport/private_fonts.h"
#include "ytec/vssrequester/windows_backend.h"
#include "ytec/windowsapp/adk_management.h"
#include "ytec/windowsapp/completion_power_ui.h"
#include "ytec/windowsapp/first_run_guidance.h"
#include "ytec/windowsapp/layout.h"
#include "ytec/windowsapp/log_retention.h"
#include "ytec/windowsapp/manual_update.h"
#include "ytec/windowsapp/media_creation.h"
#include "ytec/windowsapp/media_preflight.h"
#include "ytec/windowsapp/media_wizard.h"
#include "ytec/windowsapp/online_direct_clone.h"
#include "ytec/windowsapp/online_direct_clone_operation.h"
#include "ytec/windowsapp/online_direct_shrink_clone.h"
#include "ytec/windowsapp/online_image_create.h"
#include "ytec/windowsapp/online_image_restore_operation.h"
#include "ytec/windowsapp/online_shrink_image_product.h"
#include "ytec/windowsapp/online_shrink_image_restore.h"
#include "ytec/windowsapp/progress.h"
#include "ytec/windowsapp/restore_preflight.h"
#include "ytec/windowsapp/rescue_media_inspection.h"
#include "ytec/windowsapp/rescue_media_ui.h"
#include "ytec/windowsapp/selection.h"
#include "ytec/windowsapp/startup_data_policy.h"
#include "ytec/windowsapp/support_zip.h"
#include "ytec/windowsapp/support_zip_ui.h"
#include "ytec/windowsapp/system_state.h"
#include "ytec/windowsapp/usb_volume_mapping.h"
#include "ytec/windowsapp/windows_adk_acquisition_platform.h"
#include "ytec/windowsapp/windows_adk_management_platform.h"
#include "ytec/windowsapp/windows_data_rescue_clone.h"

#include "resource.h"

#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" NTSYSAPI NTSTATUS NTAPI RtlGetVersion(
    PRTL_OSVERSIONINFOW version_information);

namespace {

#ifndef YTEC_PRODUCT_VERSION
#error YTEC_PRODUCT_VERSION must be defined by the product build.
#endif

constexpr wchar_t kWindowClass[] = L"YtecTsumugiDriveMainWindow";
constexpr wchar_t kWindowTitle[] = L"Y-TEC Tsumugi Drive";
constexpr std::string_view kAppVersion{YTEC_PRODUCT_VERSION};
constexpr wchar_t kOfficialAdkInstallGuideUrl[] =
    L"https://learn.microsoft.com/ja-jp/windows-hardware/get-started/adk-install";
constexpr UINT kInventoryCompleteMessage = WM_APP + 1U;
constexpr UINT kBackupCompleteMessage = WM_APP + 2U;
constexpr UINT kMediaPreflightCompleteMessage = WM_APP + 3U;
constexpr UINT kRestorePreflightCompleteMessage = WM_APP + 4U;
constexpr UINT kMediaCreationProgressMessage = WM_APP + 5U;
constexpr UINT kMediaCreationCompleteMessage = WM_APP + 6U;
constexpr UINT kBackupProgressMessage = WM_APP + 7U;
constexpr UINT kCloneProgressMessage = WM_APP + 8U;
constexpr UINT kCloneCompleteMessage = WM_APP + 9U;
constexpr UINT kRestoreProgressMessage = WM_APP + 10U;
constexpr UINT kRestoreCompleteMessage = WM_APP + 11U;
constexpr UINT kManualPauseStateChangedMessage = WM_APP + 12U;
constexpr UINT kManualUpdateCompleteMessage = WM_APP + 13U;
constexpr UINT kRescueUsbInspectionCompleteMessage = WM_APP + 14U;
constexpr UINT kSupportZipPlanCompleteMessage = WM_APP + 15U;
constexpr UINT kSupportZipCreationCompleteMessage = WM_APP + 16U;
constexpr UINT kAdkManagementCompleteMessage = WM_APP + 17U;
constexpr UINT_PTR kUiRefreshTimerId = 1U;
constexpr UINT_PTR kCloneCompletionFallbackTimerId = 2U;

constexpr int kNavFirstId = 100;
constexpr int kRefreshId = 200;
constexpr int kSourceComboId = 201;
constexpr int kTargetComboId = 202;
constexpr int kPrimaryActionId = 203;
constexpr int kRestoreChangeImageId = 204;
constexpr int kMediaKindComboId = 205;
constexpr int kMediaProfileComboId = 206;
constexpr int kMediaOutputEditId = 207;
constexpr int kMediaBrowseId = 208;
constexpr int kTransferModeComboId = 209;
constexpr int kPauseActionId = 210;
constexpr int kRestoreSourcePartitionComboId = 211;
constexpr int kRestoreTargetPartitionComboId = 212;
constexpr int kManualUpdateActionId = 213;
constexpr int kFirstRunGuidanceActionId = 214;
constexpr int kImageVerificationModeComboId = 215;
constexpr int kMediaUsbModeComboId = 216;
constexpr int kMediaUsbFileSystemComboId = 217;
constexpr int kAdkOfficialDownloadInstallId = 3101;
constexpr int kAdkOfflineLayoutInstallId = 3102;
constexpr int kAdkCreateOfflineLayoutId = 3103;
constexpr int kAdkUninstallManagedId = 3104;
constexpr int kErrorCopyDetailsId = 3201;
constexpr int kCompletionPowerNoneId = 3301;
constexpr int kCompletionPowerSleepId = 3302;
constexpr int kCompletionPowerRestartId = 3303;
constexpr int kCompletionPowerShutdownId = 3304;
constexpr int kSupportZipReviewSummaryId = 3401;
constexpr int kSupportZipReviewEntriesId = 3402;
constexpr int kSupportZipReviewPrivacyId = 3403;
constexpr std::size_t kMaximumErrorDialogDetailCharacters = 240U;
static_assert(
    kMaximumErrorDialogDetailCharacters <=
        ytec::uisupport::kMaximumErrorDetailCharacters);

constexpr COLORREF kCanvas = RGB(244, 247, 249);
constexpr COLORREF kSidebar = RGB(29, 37, 49);
constexpr COLORREF kSidebarSelected = RGB(51, 64, 81);
constexpr COLORREF kInk = RGB(29, 40, 52);
constexpr COLORREF kMuted = RGB(91, 105, 118);
constexpr COLORREF kBorder = RGB(214, 222, 228);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kTsumugiBlue = RGB(30, 145, 160);
constexpr COLORREF kTsumugiPurple = RGB(121, 91, 174);
constexpr COLORREF kSafeGreen = RGB(42, 137, 93);
constexpr COLORREF kWarning = RGB(183, 112, 26);

class UiComApartment final {
 public:
  UiComApartment() noexcept
      : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

  ~UiComApartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  UiComApartment(const UiComApartment&) = delete;
  UiComApartment& operator=(const UiComApartment&) = delete;

  [[nodiscard]] bool initialized() const noexcept {
    return SUCCEEDED(result_);
  }

  [[nodiscard]] HRESULT result() const noexcept { return result_; }

 private:
  HRESULT result_{};
};

class ThreadSleepPrevention final {
 public:
  ThreadSleepPrevention() noexcept
      : previous_(SetThreadExecutionState(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED)) {}

  ~ThreadSleepPrevention() {
    if (active()) {
      // Best effort only. A destructor retry is never exposed as proof that
      // the worker released prevention before publishing its completion.
      static_cast<void>(SetThreadExecutionState(ES_CONTINUOUS));
    }
  }

  ThreadSleepPrevention(const ThreadSleepPrevention&) = delete;
  ThreadSleepPrevention& operator=(const ThreadSleepPrevention&) = delete;

  [[nodiscard]] bool active() const noexcept { return previous_ != 0U; }

  [[nodiscard]] ytec::clonecore::SleepPreventionReleaseState release()
      noexcept {
    if (release_attempted_) {
      return release_state_;
    }
    release_attempted_ = true;
    if (!active()) {
      release_state_ =
          ytec::clonecore::SleepPreventionReleaseState::unknown;
      return release_state_;
    }
    if (SetThreadExecutionState(ES_CONTINUOUS) == 0U) {
      release_state_ =
          ytec::clonecore::SleepPreventionReleaseState::release_failed;
      return release_state_;
    }
    previous_ = 0U;
    release_state_ =
        ytec::clonecore::SleepPreventionReleaseState::released;
    return release_state_;
  }

 private:
  EXECUTION_STATE previous_{};
  bool release_attempted_{};
  ytec::clonecore::SleepPreventionReleaseState release_state_{
      ytec::clonecore::SleepPreventionReleaseState::unknown};
};

enum class Page : std::uint8_t {
  clone,
  create_image,
  restore_image,
  boot_repair,
  rescue_media,
  diagnostics,
};

struct InventoryPayload final {
  std::optional<ytec::diskmodel::InventoryReport> report;
  std::wstring error;
};

struct BackupPayload final {
  std::optional<ytec::vssrequester::OnlineTsumugiBackupReport> report;
  std::optional<ytec::windowsapp::WindowsDataRescueImageCreateReport>
      rescue_report;
  std::optional<ytec::clonecore::Error> error;
  std::wstring final_path;
  bool rescue_mode{};
  std::uint64_t completion_power_operation_binding{};
  ytec::clonecore::SleepPreventionReleaseState sleep_prevention_release{
      ytec::clonecore::SleepPreventionReleaseState::unknown};
};

struct BackupProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct CloneProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct ClonePayload final {
  std::optional<
      ytec::windowsapp::OnlineDirectCloneOperationReport> report;
  std::optional<
      ytec::windowsapp::WindowsDirectShrinkCloneOperationReport>
      shrink_report;
  std::optional<
      ytec::windowsapp::WindowsDataRescueCloneOperationReport>
      rescue_report;
  std::optional<ytec::clonecore::Error> error;
  std::uint32_t target_disk_number{};
  bool rescue_mode{};
  bool shrink_mode{};
  std::uint64_t completion_power_operation_binding{};
  ytec::clonecore::SleepPreventionReleaseState sleep_prevention_release{
      ytec::clonecore::SleepPreventionReleaseState::unknown};
};

struct MediaPreflightPayload final {
  ytec::windowsapp::MediaPreflightView view;
};

struct MediaCreationProgressPayload final {
  ytec::windowsapp::RescueMediaCreationProgress progress;
};

struct MediaCreationPayload final {
  std::optional<ytec::windowsapp::RescueMediaCreationReport> report;
  std::optional<ytec::clonecore::Error> error;
  ytec::windowsapp::RescueMediaKind requested_kind{
      ytec::windowsapp::RescueMediaKind::iso_file};
  std::uint64_t completion_power_operation_binding{};
  ytec::clonecore::SleepPreventionReleaseState sleep_prevention_release{
      ytec::clonecore::SleepPreventionReleaseState::unknown};
};

struct RescueUsbInspectionPayload final {
  ytec::clonecore::StableDiskIdentity expected_target;
  ytec::windowsapp::RescueUsbCanonicalLayout expected_layout;
  ytec::windowsapp::RescueUsbInspectionResult result;
};

struct RestorePreflightPayload final {
  std::optional<
      ytec::windowsapp::TsumugiRestoreImagePreflightReport> report;
  std::optional<ytec::clonecore::Error> error;
};

struct RestoreProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct RestorePayload final {
  std::optional<
      ytec::windowsapp::OnlineImageRestoreOperationReport> report;
  std::optional<
      ytec::windowsapp::WindowsOnlineShrinkRestoreOperationReport>
      shrink_report;
  std::optional<ytec::clonecore::Error> error;
  std::uint32_t target_disk_number{};
  bool individual_partition{};
  std::uint64_t completion_power_operation_binding{};
  ytec::clonecore::SleepPreventionReleaseState sleep_prevention_release{
      ytec::clonecore::SleepPreventionReleaseState::unknown};
};

struct ManualUpdatePayload final {
  std::optional<ytec::windowsapp::ManualUpdateCheckReport> report;
  std::optional<ytec::clonecore::Error> error;
};

struct SupportZipPlanPayload final {
  std::optional<ytec::windowsapp::SupportZipPlan> plan;
  std::optional<ytec::clonecore::Error> error;
};

struct SupportZipCreationPayload final {
  std::optional<ytec::windowsapp::SupportZipCreationReport> report;
  std::optional<ytec::clonecore::Error> error;
};

struct AdkManagementPayload final {
  std::optional<ytec::windowsapp::AdkManagementReport> report;
  std::optional<ytec::clonecore::Error> error;
};

struct ProductLogSession final {
  ytec::clonecore::Logger logger;
  std::wstring path;
  std::shared_ptr<std::atomic_bool> error_detected;
  std::size_t retention_deleted_count{};
  std::uint64_t retention_deleted_bytes{};
};

struct AppState final {
  HWND window{};
  std::array<HWND, 6> navigation{};
  HWND refresh{};
  HWND source_combo{};
  HWND target_combo{};
  HWND transfer_mode_combo{};
  HWND image_verification_mode_combo{};
  HWND restore_source_partition_combo{};
  HWND restore_target_partition_combo{};
  HWND restore_change_image{};
  HWND media_kind_combo{};
  HWND media_profile_combo{};
  HWND media_usb_mode_combo{};
  HWND media_usb_file_system_combo{};
  HWND media_output_edit{};
  HWND media_browse{};
  HWND primary_action{};
  HWND pause_action{};
  HWND manual_update_action{};
  HWND first_run_guidance_action{};
  HFONT body_font{};
  HFONT small_font{};
  HFONT heading_font{};
  HFONT brand_font{};
  ytec::uisupport::PrivateFontCollection private_fonts;
  Page page{Page::clone};
  ytec::clonecore::BoundedRamLogRouter log_router;
  std::optional<ytec::clonecore::Logger> logger;
  std::wstring log_path;
  std::wstring log_error;
  std::wstring first_run_guidance_status;
  std::shared_ptr<std::atomic_bool> persistent_log_error_detected;
  std::optional<ytec::clonecore::StableDiskIdentity>
      persistent_log_backing_identity;
  ytec::windowsapp::StartupDataPolicy startup_data_policy;
  std::optional<ytec::diskmodel::InventoryReport> inventory;
  std::wstring inventory_error;
  std::atomic_bool inventory_loading{false};
  std::thread inventory_thread;
  std::atomic_bool clone_running{false};
  std::atomic_bool clone_cancel_requested{false};
  std::atomic_bool clone_completion_post_failed{false};
  std::optional<ytec::clonecore::DiskOperationProgress> clone_progress;
  std::chrono::milliseconds clone_elapsed{};
  std::thread clone_thread;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      clone_pause_controller;
  bool active_clone_is_rescue{};
  bool active_clone_is_shrink{};
  std::atomic_bool backup_running{false};
  std::atomic_bool backup_cancel_requested{false};
  std::optional<ytec::clonecore::DiskOperationProgress> backup_progress;
  std::chrono::milliseconds backup_elapsed{};
  std::thread backup_thread;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      backup_pause_controller;
  std::atomic_bool media_preflight_running{false};
  std::optional<ytec::windowsapp::MediaPreflightView> media_preflight;
  std::thread media_preflight_thread;
  std::atomic_bool media_creation_running{false};
  std::atomic_bool media_creation_cancel_requested{false};
  std::optional<ytec::windowsapp::RescueMediaCreationProgress>
      media_creation_progress;
  std::optional<ytec::windowsapp::RescueMediaCreationReport>
      media_creation_report;
  std::optional<ytec::windowsapp::RescueMediaCreationStage>
      last_logged_media_stage;
  ULONGLONG media_creation_started_tick{};
  std::thread media_creation_thread;
  std::atomic_bool media_usb_inspection_running{false};
  std::atomic_bool media_usb_inspection_cancel_requested{false};
  std::optional<ytec::windowsapp::RescueUsbInspectionResult>
      media_usb_inspection;
  std::thread media_usb_inspection_thread;
  std::atomic_bool restore_preflight_running{false};
  std::atomic_bool restore_preflight_cancel_requested{false};
  std::optional<
      ytec::windowsapp::TsumugiRestoreImagePreflightReport>
      restore_preflight;
  std::vector<std::uint32_t> restore_source_partition_candidates;
  std::vector<ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection>
      restore_target_partition_candidates;
  std::thread restore_preflight_thread;
  std::atomic_bool restore_running{false};
  std::atomic_bool restore_cancel_requested{false};
  std::optional<ytec::clonecore::DiskOperationProgress> restore_progress;
  std::chrono::milliseconds restore_elapsed{};
  std::thread restore_thread;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      restore_pause_controller;
  std::atomic_bool manual_update_running{false};
  std::optional<ytec::windowsapp::ManualUpdateCheckReport>
      manual_update_report;
  std::wstring manual_update_error;
  std::thread manual_update_thread;
  std::atomic_bool support_zip_planning{false};
  std::atomic_bool support_zip_creation_running{false};
  std::optional<ytec::windowsapp::SupportZipCreationReport>
      support_zip_report;
  std::wstring support_zip_error;
  std::wstring support_zip_status;
  std::thread support_zip_thread;
  std::atomic_bool adk_management_running{false};
  std::wstring adk_management_status;
  std::thread adk_management_thread;
  std::uint64_t next_completion_power_operation_binding{1U};
  bool clean_close_requested{};
  bool elevated{};
};

std::shared_ptr<ytec::clonecore::ManualPauseController>
make_ui_manual_pause_controller(const HWND window) {
  auto last_ui_state = std::make_shared<std::atomic<std::uint16_t>>(
      (std::numeric_limits<std::uint16_t>::max)());
  return std::make_shared<ytec::clonecore::ManualPauseController>(
      [window, last_ui_state](
          const ytec::clonecore::ManualPauseSnapshot& snapshot) {
        const auto key = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(snapshot.state) << 8U) |
            static_cast<std::uint16_t>(snapshot.availability));
        if (last_ui_state->exchange(key) == key) {
          return;
        }
        static_cast<void>(PostMessageW(
            window, kManualPauseStateChangedMessage, 0, 0));
      });
}

struct SupportZipReviewDialogState final {
  const ytec::windowsapp::SupportZipReviewModel* model{};
  const ytec::windowsapp::SupportZipReviewLayout* layout{};
  HFONT font{};
};

INT_PTR CALLBACK support_zip_review_dialog_proc(
    const HWND dialog,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* review = reinterpret_cast<SupportZipReviewDialogState*>(
      GetWindowLongPtrW(dialog, DWLP_USER));
  switch (message) {
    case WM_INITDIALOG: {
      review = reinterpret_cast<SupportZipReviewDialogState*>(lparam);
      if (review == nullptr || review->model == nullptr ||
          review->layout == nullptr) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      SetWindowLongPtrW(
          dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(review));
      const auto& layout = *review->layout;
      RECT window_bounds{
          0, 0, layout.client_width, layout.client_height};
      static_cast<void>(AdjustWindowRectEx(
          &window_bounds,
          static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
          FALSE,
          static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE))));
      RECT work_area{};
      const HWND owner = GetWindow(dialog, GW_OWNER);
      MONITORINFO monitor_info{.cbSize = sizeof(MONITORINFO)};
      const HMONITOR monitor = MonitorFromWindow(
          owner != nullptr ? owner : dialog,
          MONITOR_DEFAULTTONEAREST);
      if (monitor != nullptr &&
          GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
        work_area = monitor_info.rcWork;
      } else {
        work_area = RECT{
            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      }
      const int dialog_width = window_bounds.right - window_bounds.left;
      const int dialog_height = window_bounds.bottom - window_bounds.top;
      const int dialog_left = work_area.left +
          ((work_area.right - work_area.left) - dialog_width) / 2;
      const int dialog_top = work_area.top +
          ((work_area.bottom - work_area.top) - dialog_height) / 2;
      SetWindowPos(
          dialog,
          nullptr,
          dialog_left,
          dialog_top,
          dialog_width,
          dialog_height,
          SWP_NOZORDER | SWP_NOACTIVATE);
      const auto make_control =
          [dialog, review](
              const wchar_t* class_name,
              const wchar_t* text,
              const DWORD style,
              const int identifier,
              const ytec::windowsapp::SupportZipUiBounds& bounds) {
            const bool list_box = std::wstring_view(class_name) == L"LISTBOX";
            const HWND control = CreateWindowExW(
                list_box ? WS_EX_CLIENTEDGE : 0,
                class_name,
                text,
                WS_CHILD | WS_VISIBLE | style,
                bounds.left,
                bounds.top,
                bounds.width(),
                bounds.height(),
                dialog,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(identifier)),
                nullptr,
                nullptr);
            if (control != nullptr && review->font != nullptr) {
              SendMessageW(
                  control,
                  WM_SETFONT,
                  reinterpret_cast<WPARAM>(review->font),
                  TRUE);
            }
            return control;
          };
      const HWND summary = make_control(
          L"STATIC",
          review->model->summary.c_str(),
          SS_LEFT,
          kSupportZipReviewSummaryId,
          layout.summary);
      const HWND entries = make_control(
          L"LISTBOX",
          L"",
          LBS_NOINTEGRALHEIGHT | LBS_USETABSTOPS | WS_VSCROLL |
              WS_TABSTOP,
          kSupportZipReviewEntriesId,
          layout.entries);
      const HWND privacy = make_control(
          L"STATIC",
          review->model->privacy_notice.c_str(),
          SS_LEFT,
          kSupportZipReviewPrivacyId,
          layout.privacy_notice);
      const HWND create = make_control(
          L"BUTTON",
          ytec::windowsapp::support_zip_ui_contract().create_label.data(),
          BS_DEFPUSHBUTTON | WS_TABSTOP,
          IDOK,
          layout.create_button);
      const HWND cancel = make_control(
          L"BUTTON",
          L"キャンセル",
          BS_PUSHBUTTON | WS_TABSTOP,
          IDCANCEL,
          layout.cancel_button);
      if (summary == nullptr || entries == nullptr || privacy == nullptr ||
          create == nullptr || cancel == nullptr) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      for (const auto& row : review->model->entry_rows) {
        SendMessageW(
            entries,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(row.c_str()));
      }
      SetFocus(create);
      return FALSE;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, LOWORD(wparam));
        return TRUE;
      }
      break;
    case WM_CLOSE:
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    default:
      break;
  }
  return FALSE;
}

bool review_support_zip_model(
    const HWND owner,
    const HFONT font,
    const ytec::windowsapp::SupportZipReviewModel& review_model) {
  RECT owner_area{};
  if (GetWindowRect(owner, &owner_area) == FALSE) {
    owner_area = RECT{0, 0, 1024, 600};
  }
  HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{.cbSize = sizeof(MONITORINFO)};
  if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
    owner_area = monitor_info.rcWork;
  }
  const auto layout =
      ytec::windowsapp::calculate_support_zip_review_layout(
          owner_area.right - owner_area.left,
          owner_area.bottom - owner_area.top);
  const int frame_width = GetSystemMetrics(SM_CXSIZEFRAME) * 2 +
      GetSystemMetrics(SM_CXPADDEDBORDER) * 2;
  const int frame_height = GetSystemMetrics(SM_CYSIZEFRAME) * 2 +
      GetSystemMetrics(SM_CXPADDEDBORDER) * 2 +
      GetSystemMetrics(SM_CYCAPTION);
  const int available_width = owner_area.right - owner_area.left;
  const int available_height = owner_area.bottom - owner_area.top;
  if (layout.client_width + frame_width >
          available_width - ytec::windowsapp::kSupportZipReviewOuterGap ||
      layout.client_height + frame_height >
          available_height - ytec::windowsapp::kSupportZipReviewOuterGap) {
    return false;
  }
  alignas(DWORD) std::array<std::byte, 256> dialog_storage{};
  auto* dialog_template = reinterpret_cast<DLGTEMPLATE*>(
      dialog_storage.data());
  dialog_template->style =
      WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
  dialog_template->dwExtendedStyle = 0;
  dialog_template->cdit = 0;
  dialog_template->x = 0;
  dialog_template->y = 0;
  dialog_template->cx = static_cast<short>(layout.client_width);
  dialog_template->cy = static_cast<short>(layout.client_height);
  WORD* template_words = reinterpret_cast<WORD*>(dialog_template + 1);
  *template_words++ = 0;  // no menu
  *template_words++ = 0;  // default dialog class
  constexpr std::wstring_view kReviewTitle{L"サポートZIPの含有一覧"};
  for (const wchar_t value : kReviewTitle) {
    *template_words++ = static_cast<WORD>(value);
  }
  *template_words = 0;
  SupportZipReviewDialogState review{
      .model = &review_model,
      .layout = &layout,
      .font = font,
  };
  const HWND previous_focus = GetFocus();
  const INT_PTR result = DialogBoxIndirectParamW(
      GetModuleHandleW(nullptr),
      dialog_template,
      owner,
      support_zip_review_dialog_proc,
      reinterpret_cast<LPARAM>(&review));
  if (previous_focus != nullptr && IsWindow(previous_focus) != FALSE) {
    SetFocus(previous_focus);
  }
  return result == IDOK;
}

bool review_support_zip_plan(
    const HWND owner,
    const HFONT font,
    const ytec::windowsapp::SupportZipPlan& plan) {
  const auto review_model =
      ytec::windowsapp::build_support_zip_review_model(
          plan.final_path(),
          plan.entries(),
          plan.masked_total_bytes(),
          plan.excluded_log_count());
  return review_support_zip_model(owner, font, review_model);
}

void update_manual_pause_button(
    const HWND button,
    const bool operation_running,
    const std::shared_ptr<ytec::clonecore::ManualPauseController>&
        controller) {
  ShowWindow(button, operation_running ? SW_SHOW : SW_HIDE);
  if (!operation_running || controller == nullptr) {
    EnableWindow(button, FALSE);
    SetWindowTextW(button, L"一時停止不可");
    return;
  }

  const auto snapshot = controller->snapshot();
  std::wstring_view label = L"一時停止不可";
  bool enabled = false;
  switch (snapshot.state) {
    case ytec::clonecore::ManualPauseState::running:
      enabled = snapshot.availability ==
          ytec::clonecore::ManualPauseAvailability::
              available_at_safe_boundary;
      label = enabled ? L"一時停止" : L"一時停止不可";
      break;
    case ytec::clonecore::ManualPauseState::pause_requested:
      label = L"停止要求を取り消す";
      enabled = true;
      break;
    case ytec::clonecore::ManualPauseState::paused:
      label = L"再開";
      enabled = true;
      break;
    case ytec::clonecore::ManualPauseState::cancelling:
      label = L"取消処理中";
      break;
    case ytec::clonecore::ManualPauseState::completed:
      label = L"完了";
      break;
  }
  SetWindowTextW(button, std::wstring(label).c_str());
  EnableWindow(button, enabled ? TRUE : FALSE);
}

bool toggle_manual_pause(
    const std::shared_ptr<ytec::clonecore::ManualPauseController>&
        controller) noexcept {
  if (controller == nullptr) {
    return false;
  }
  const auto snapshot = controller->snapshot();
  if (snapshot.state == ytec::clonecore::ManualPauseState::running) {
    return controller->request_pause();
  }
  if (snapshot.state ==
          ytec::clonecore::ManualPauseState::pause_requested ||
      snapshot.state == ytec::clonecore::ManualPauseState::paused) {
    return controller->resume();
  }
  return false;
}

std::wstring diagnostic_only_message(const AppState& state) {
  std::wstring message =
      L"この起動では書き込み／破壊操作を行いません。\n\n"
      L"EXEと同じフォルダーの「data」を、通常の非reparseディレクトリとして作成し、書込み・flush・読戻し・削除まで確認できなかったため、診断専用モードで起動しています。\n"
      L"AppDataなど別の保存先へ自動退避はしません。";
  if (!state.startup_data_policy.diagnostic.empty()) {
    message += L"\n\n理由: " + state.startup_data_policy.diagnostic;
  }
  if (state.startup_data_policy.native_code != ERROR_SUCCESS) {
    message += L"\nWindows error: " +
               std::to_wstring(state.startup_data_policy.native_code);
  }
  return message;
}

bool require_startup_write_access(
    AppState& state,
    const std::wstring_view operation) {
  if (state.startup_data_policy.write_operations_permitted()) {
    return true;
  }
  if (state.logger.has_value()) {
    state.logger->error(
        std::wstring(operation) + L"を診断専用ゲートで停止");
  }
  MessageBoxW(
      state.window,
      diagnostic_only_message(state).c_str(),
      L"診断専用モード",
      MB_OK | MB_ICONWARNING);
  return false;
}

bool confirm_long_operation_power(
    AppState& state,
    const std::wstring_view operation) {
  const auto observation =
      ytec::windowsapp::query_windows_power_observation();
  const auto advisory =
      ytec::windowsapp::evaluate_long_operation_power(observation);
  if (!advisory.additional_confirmation_required) {
    return true;
  }
  if (state.logger.has_value()) {
    state.logger->warning(
        std::wstring(operation) +
        L"の開始前に電源状態の追加確認が必要 battery_percent=" +
        (observation.battery_percent.has_value()
             ? std::to_wstring(observation.battery_percent.value())
             : L"unknown") +
        L" native_status=" + std::to_wstring(observation.native_status));
  }
  const std::wstring message =
      std::wstring(operation) +
      L"は長時間かかる場合があります。\n\n" + advisory.message +
      L"\n\nこの電源状態を確認したうえで続けますか？";
  return MessageBoxW(
             state.window,
             message.c_str(),
             L"電源状態の追加確認",
             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

struct ConfirmationDialogState final {
  std::wstring details;
  std::wstring token;
  std::wstring confirm_button_label{L"OK"};
  HFONT font{};
};

void erase_secret(std::string& value) noexcept {
  if (!value.empty()) {
    SecureZeroMemory(value.data(), value.size());
    value.clear();
  }
}

class SecureAsciiPassword final {
 public:
  explicit SecureAsciiPassword(const std::string_view value)
      : bytes_(value.begin(), value.end()) {}

  ~SecureAsciiPassword() {
    if (!bytes_.empty()) {
      SecureZeroMemory(bytes_.data(), bytes_.size());
    }
  }

  SecureAsciiPassword(const SecureAsciiPassword&) = delete;
  SecureAsciiPassword& operator=(const SecureAsciiPassword&) = delete;

  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(bytes_.data(), bytes_.size());
  }

 private:
  std::vector<char> bytes_;
};

struct TsumugiPasswordDialogState final {
  std::wstring prompt;
  std::wstring title;
  std::wstring confirm_button_label{L"続行"};
  HFONT font{};
  bool require_confirmation{};
  bool allow_empty{};
  std::string password;

  ~TsumugiPasswordDialogState() { erase_secret(password); }
};

struct TsumugiPasswordPromptResult final {
  bool accepted{};
  std::shared_ptr<SecureAsciiPassword> password;
};

constexpr std::array<std::wstring_view, 6> kNavigationLabels{
    L"ドライブをクローン",
    L"イメージを作成",
    L"イメージを復元",
    L"起動を修復",
    L"レスキューメディア",
    L"ログ・診断",
};

std::wstring format_bytes(const std::uint64_t bytes) {
  return ytec::windowsapp::format_bytes(bytes);
}

std::wstring partition_style_text(
    const ytec::diskmodel::PartitionStyle style) {
  switch (style) {
    case ytec::diskmodel::PartitionStyle::gpt:
      return L"GPT";
    case ytec::diskmodel::PartitionStyle::mbr:
      return L"MBR";
    case ytec::diskmodel::PartitionStyle::raw:
      return L"未初期化";
    case ytec::diskmodel::PartitionStyle::unknown:
      return L"不明";
  }
  return L"不明";
}

std::wstring disk_label(const ytec::diskmodel::DiskInfo& disk) {
  std::wstring label =
      L"ディスク " + std::to_wstring(disk.disk_number) + L"  ";
  label += disk.model.empty() ? L"モデル不明" : disk.model;
  label += L"  (" + format_bytes(disk.size_bytes) + L")";
  if (disk.is_system_disk) {
    label += L"  [Windows]";
  }
  label += L"  [健康: " +
           std::wstring(ytec::diskmodel::disk_health_state_name(
               disk.health.state));
  if (disk.health.temperature_celsius.has_value()) {
    label += L" / " +
             std::to_wstring(disk.health.temperature_celsius.value()) +
             L"°C";
  }
  if (disk.health.temperature_warning) {
    label += L" 温度警告";
  }
  label += L"]";
  return label;
}

std::wstring disk_health_summary(
    const ytec::diskmodel::DiskInfo& disk) {
  std::wstring summary = L"健康: " + std::wstring(
      ytec::diskmodel::disk_health_state_name(disk.health.state));
  if (disk.health.temperature_celsius.has_value()) {
    summary += L" / " +
               std::to_wstring(disk.health.temperature_celsius.value()) +
               L"°C";
  } else {
    summary += L" / 温度未取得";
  }
  if (disk.health.temperature_warning) {
    summary += L"（温度警告）";
  }
  return summary;
}

bool require_healthy_write_target(
    AppState& state,
    const ytec::diskmodel::DiskInfo& target,
    const std::wstring_view operation) {
  if (ytec::diskmodel::disk_health_operation_advice(
          target.health, false) !=
      ytec::diskmodel::DiskHealthOperationAdvice::block_target) {
    if (!target.health.temperature_warning) {
      return true;
    }
    const std::wstring message =
        std::wstring(operation) +
        L"の対象ディスクは温度警告を報告しています。\n\n" +
        disk_health_summary(target) +
        L"\n\n温度だけでは自動停止しませんが、冷却と安定した電源を確認してから続けることを推奨します。続けますか？";
    return MessageBoxW(
               state.window,
               message.c_str(),
               L"ディスク温度の警告",
               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
  }
  if (state.logger.has_value()) {
    state.logger->error(
        std::wstring(operation) +
        L"をコピー先健康状態ゲートで停止 state=" +
        std::wstring(ytec::diskmodel::disk_health_state_name(
            target.health.state)));
  }
  const std::wstring message =
      std::wstring(operation) +
      L"を開始できません。\n\nコピー先のSMARTまたはNVMe健康状態が「注意／異常」です。異常を報告しているディスクへ新しいクローンや復元を書き込むことはできません。\n\n" +
      disk_health_summary(target);
  MessageBoxW(
      state.window,
      message.c_str(),
      L"コピー先ディスクの健康状態",
      MB_OK | MB_ICONERROR);
  return false;
}

bool confirm_source_health_advice(
    AppState& state,
    const ytec::diskmodel::DiskInfo& source,
    const std::wstring_view operation) {
  const bool rescue_recommended =
      ytec::diskmodel::disk_health_operation_advice(
          source.health, true) ==
      ytec::diskmodel::DiskHealthOperationAdvice::recommend_rescue;
  if (!rescue_recommended && !source.health.temperature_warning) {
    return true;
  }
  if (state.logger.has_value()) {
    state.logger->warning(
        std::wstring(operation) +
        L"のコピー元健康状態に警告 state=" +
        std::wstring(ytec::diskmodel::disk_health_state_name(
            source.health.state)) +
        L" temperature_warning=" +
        (source.health.temperature_warning ? L"true" : L"false"));
  }
  std::wstring message =
      std::wstring(operation) + L"のコピー元に警告があります。\n\n" +
      disk_health_summary(source);
  if (rescue_recommended) {
    message +=
        L"\n\n通常モードは読取りエラーで停止します。欠損マップと小ブロック再試行を使う救出モードを推奨します。システムディスクの救出はPE版だけで扱います。";
  }
  message +=
      L"\n\n警告を確認したうえで、この通常処理を続けますか？";
  return MessageBoxW(
             state.window,
             message.c_str(),
             L"コピー元ディスクの警告",
             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

std::wstring format_error_message(
    const ytec::clonecore::Error& error) {
  return ytec::uisupport::format_error_primary(
      ytec::uisupport::make_error_presentation(error));
}

bool copy_error_details_to_clipboard(
    const HWND owner,
    const std::wstring_view text) {
  if (text.empty() || OpenClipboard(owner) == FALSE) {
    return false;
  }
  struct ClipboardCloser final {
    ~ClipboardCloser() { CloseClipboard(); }
  } clipboard_closer;
  if (EmptyClipboard() == FALSE ||
      text.size() >=
          (std::numeric_limits<std::size_t>::max)() /
              sizeof(wchar_t) -
              1U) {
    return false;
  }
  const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    return false;
  }
  void* const destination = GlobalLock(memory);
  if (destination == nullptr) {
    GlobalFree(memory);
    return false;
  }
  const errno_t copied = memcpy_s(
      destination,
      bytes,
      text.data(),
      text.size() * sizeof(wchar_t));
  if (copied == 0) {
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
  }
  GlobalUnlock(memory);
  if (copied != 0 ||
      SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    return false;
  }
  return true;
}

struct ErrorTaskDialogContext final {
  std::wstring copy_text;
};

std::wstring bounded_error_detail_preview(
    const std::wstring_view details) {
  if (details.size() <= kMaximumErrorDialogDetailCharacters) {
    return std::wstring(details);
  }
  constexpr std::wstring_view marker =
      L"\r\n＜続きは「サポート用の詳細をコピー」で確認できます＞";
  const std::size_t prefix_length =
      kMaximumErrorDialogDetailCharacters > marker.size()
      ? kMaximumErrorDialogDetailCharacters - marker.size()
      : 0U;
  std::wstring preview(details.substr(0U, prefix_length));
  preview += marker;
  return preview;
}

HRESULT CALLBACK error_task_dialog_callback(
    const HWND dialog,
    const UINT notification,
    const WPARAM wparam,
    const LPARAM,
    const LONG_PTR reference_data) noexcept {
  if (notification != TDN_BUTTON_CLICKED ||
      static_cast<int>(wparam) != kErrorCopyDetailsId) {
    return S_OK;
  }
  const auto* context = reinterpret_cast<const ErrorTaskDialogContext*>(
      reference_data);
  const bool copied = context != nullptr &&
      copy_error_details_to_clipboard(dialog, context->copy_text);
  SendMessageW(
      dialog,
      TDM_SET_ELEMENT_TEXT,
      TDE_FOOTER,
      reinterpret_cast<LPARAM>(
          copied
              ? L"マスク済みの詳細をクリップボードへコピーしました。"
              : L"詳細をコピーできませんでした。もう一度お試しください。"));
  return S_FALSE;
}

void restore_error_dialog_focus(const HWND previous_focus) noexcept {
  if (previous_focus != nullptr && IsWindow(previous_focus) != FALSE &&
      IsWindowEnabled(previous_focus) != FALSE &&
      IsWindowVisible(previous_focus) != FALSE) {
    SetFocus(previous_focus);
  }
}

std::wstring first_run_guidance_content() {
  std::wstring content;
  for (std::size_t index = 0U;
       index < ytec::windowsapp::kFirstRunGuidanceItems.size();
       ++index) {
    if (!content.empty()) {
      content += L"\r\n\r\n";
    }
    const auto& item = ytec::windowsapp::kFirstRunGuidanceItems[index];
    content += std::to_wstring(index + 1U) + L". " +
               std::wstring(item.title) + L"\r\n" +
               std::wstring(item.description);
  }
  return content;
}

bool show_first_run_guidance_dialog(
    AppState& state,
    const bool first_run) {
  const HWND previous_focus = GetFocus();
  const std::wstring content = first_run_guidance_content();
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = state.window;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_SIZE_TO_CONTENT;
  config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = L"Y-TEC Tsumugi Drive 安全ガイド";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction = L"はじめに、3つだけ確認してください";
  config.pszContent = content.c_str();
  config.nDefaultButton = IDOK;
  config.pszFooterIcon = TD_INFORMATION_ICON;
  config.pszFooter = first_run
      ? L"［OK］で次回から省略します。［閉じる］でも主要画面をそのまま使用できます。"
      : L"初回と同じ案内です。ここで設定は変更しません。";

  using TaskDialogIndirectFunction = HRESULT(WINAPI*)(
      const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE common_controls = LoadLibraryExW(
      L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  TaskDialogIndirectFunction task_dialog{};
  if (common_controls != nullptr) {
    task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
        GetProcAddress(common_controls, "TaskDialogIndirect"));
    if (task_dialog == nullptr) {
      task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
          GetProcAddress(common_controls, MAKEINTRESOURCEA(345)));
    }
  }
  int pressed_button = IDCLOSE;
  const HRESULT shown = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed_button, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  bool acknowledged = SUCCEEDED(shown) && pressed_button == IDOK;
  if (FAILED(shown)) {
    if (state.logger.has_value()) {
      state.logger->warning(
          L"初回安全ガイドTaskDialogを表示できないため設定を変更しません");
    }
    MessageBoxW(
        state.window,
        content.c_str(),
        L"はじめに確認する3項目",
        MB_OK | MB_ICONINFORMATION);
    acknowledged = false;
  }
  restore_error_dialog_focus(previous_focus);
  if (previous_focus == nullptr &&
      state.navigation[static_cast<std::size_t>(state.page)] != nullptr) {
    SetFocus(state.navigation[static_cast<std::size_t>(state.page)]);
  }
  return acknowledged;
}

void show_first_run_guidance_if_needed(AppState& state) {
  const auto inspection =
      ytec::windowsapp::inspect_windows_first_run_guidance();
  if (!inspection.diagnostic.empty()) {
    state.first_run_guidance_status =
        L"初回安全ガイド設定: " + inspection.diagnostic;
  }
  if (!inspection.decision.show_guidance) {
    if (state.logger.has_value()) {
      state.logger->info(
          L"初回安全ガイド schema=1 acknowledged=true");
    }
    return;
  }
  if (!show_first_run_guidance_dialog(state, true)) {
    if (state.logger.has_value()) {
      state.logger->info(
          L"初回安全ガイド acknowledged=false persistence=unchanged");
    }
    return;
  }
  if (!inspection.decision.acknowledgement_may_be_saved) {
    if (state.first_run_guidance_status.empty()) {
      state.first_run_guidance_status =
          L"初回安全ガイド設定を安全に更新できないため、次回も案内します。";
    }
    if (state.logger.has_value()) {
      state.logger->warning(
          L"初回安全ガイド acknowledged=true persistence=blocked");
    }
    return;
  }
  auto saved = ytec::windowsapp::
      save_windows_first_run_guidance_acknowledgement();
  if (!saved) {
    state.first_run_guidance_status =
        L"初回安全ガイド設定を保存できなかったため、次回も案内します。理由: " +
        format_error_message(saved.error());
    if (state.logger.has_value()) {
      state.logger->warning(
          L"初回安全ガイド acknowledged=true persistence=failed code=" +
          std::wstring(
              ytec::clonecore::error_code_name(saved.error().code)) +
          L" native_code=" +
          std::to_wstring(saved.error().native_code));
    }
    return;
  }
  state.first_run_guidance_status =
      saved.value().recovery_backup_retained
      ? L"初回安全ガイド設定は保存しました。復旧用backupを保持しています。"
      : L"初回安全ガイド設定をEXE隣dataへ保存しました。";
  if (state.logger.has_value()) {
    state.logger->info(
        L"初回安全ガイド acknowledged=true persistence=saved schema=1");
  }
}

void show_product_error(
    const HWND owner,
    const std::wstring_view title,
    const ytec::clonecore::Error& error,
    const std::wstring_view safety_guidance = {}) {
  const auto presentation =
      ytec::uisupport::make_error_presentation(error);
  const HWND previous_focus = GetFocus();
  std::wstring primary =
      L"コード: " + presentation.code +
      L"\r\n次の操作: " + presentation.next_action;
  if (!safety_guidance.empty()) {
    primary += L"\r\n\r\n";
    primary.append(safety_guidance);
  }
  const std::wstring expanded_details =
      bounded_error_detail_preview(presentation.details);
  ErrorTaskDialogContext context{
      .copy_text =
          ytec::uisupport::format_error_details_for_copy(presentation),
  };
  constexpr TASKDIALOG_BUTTON kCopyButton{
      kErrorCopyDetailsId,
      L"サポート用の詳細をコピー"};
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_SIZE_TO_CONTENT;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  const std::wstring window_title(title);
  config.pszWindowTitle = window_title.c_str();
  config.pszMainIcon = TD_ERROR_ICON;
  config.pszMainInstruction = presentation.summary.c_str();
  config.pszContent = primary.c_str();
  if (presentation.details_expandable && !expanded_details.empty()) {
    config.pszExpandedInformation = expanded_details.c_str();
    config.pszExpandedControlText = L"詳細を表示";
    config.pszCollapsedControlText = L"詳細を閉じる";
  }
  if (presentation.details_copyable && !context.copy_text.empty()) {
    config.cButtons = 1U;
    config.pButtons = &kCopyButton;
  }
  config.nDefaultButton = IDCLOSE;
  config.pszFooterIcon = TD_INFORMATION_ICON;
  config.pszFooter =
      L"詳細は安全な上限内に整形され、秘密値と長いパスは省略されます。";
  config.pfCallback = error_task_dialog_callback;
  config.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);

  using TaskDialogIndirectFunction = HRESULT(WINAPI*)(
      const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE common_controls = LoadLibraryExW(
      L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  TaskDialogIndirectFunction task_dialog{};
  if (common_controls != nullptr) {
    task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
        GetProcAddress(common_controls, "TaskDialogIndirect"));
    if (task_dialog == nullptr) {
      task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
          GetProcAddress(common_controls, MAKEINTRESOURCEA(345)));
    }
  }
  const HRESULT shown = task_dialog == nullptr
      ? E_NOTIMPL
      : task_dialog(&config, nullptr, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(shown)) {
    std::wstring fallback =
        ytec::uisupport::format_error_primary(presentation);
    if (!safety_guidance.empty()) {
      fallback += L"\r\n\r\n";
      fallback.append(safety_guidance);
    }
    fallback +=
        L"\r\n\r\n詳細表示を利用できません。"
        L"マスク済みの詳細をクリップボードへコピーしますか？";
    if (MessageBoxW(
            owner,
            fallback.c_str(),
            window_title.c_str(),
            MB_YESNO | MB_ICONERROR | MB_DEFBUTTON2) == IDYES) {
      const bool copied = copy_error_details_to_clipboard(
          owner, context.copy_text);
      MessageBoxW(
          owner,
          copied
              ? L"マスク済みの詳細をクリップボードへコピーしました。"
              : L"クリップボードへコピーできませんでした。",
          window_title.c_str(),
          MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
  }
  restore_error_dialog_focus(previous_focus);
}

std::wstring widen_ascii(const std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

std::string current_utc_timestamp() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  std::array<char, 21> buffer{};
  const int written = sprintf_s(
      buffer.data(),
      buffer.size(),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      static_cast<unsigned int>(time.wYear),
      static_cast<unsigned int>(time.wMonth),
      static_cast<unsigned int>(time.wDay),
      static_cast<unsigned int>(time.wHour),
      static_cast<unsigned int>(time.wMinute),
      static_cast<unsigned int>(time.wSecond));
  return written == 20 ? std::string(buffer.data(), 20) : std::string{};
}

ytec::clonecore::Result<std::array<std::uint32_t, 3>>
current_windows_version() {
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  const NTSTATUS status = RtlGetVersion(&version);
  if (status < 0) {
    return ytec::clonecore::Result<
        std::array<std::uint32_t, 3>>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::query_failed,
            .native_code = RtlNtStatusToDosError(status),
            .operation = L"Windowsバージョン取得",
            .message =
                L"バックアップマニフェストへ記録するWindows版を取得できません",
        });
  }
  return ytec::clonecore::Result<
      std::array<std::uint32_t, 3>>::success({
      version.dwMajorVersion,
      version.dwMinorVersion,
      version.dwBuildNumber,
  });
}

std::string current_native_architecture() {
  SYSTEM_INFO information{};
  GetNativeSystemInfo(&information);
  if (information.wProcessorArchitecture ==
      PROCESSOR_ARCHITECTURE_AMD64) {
    return "AMD64";
  }
  return {};
}

ytec::clonecore::Result<ProductLogSession>
create_product_log_session(const std::wstring& verified_data_directory) {
  if (verified_data_directory.empty()) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::invalid_argument,
        .native_code = ERROR_INVALID_NAME,
        .operation = L"ログ保存場所の確認",
        .message = L"書込み確認済みdataフォルダーが指定されていません",
    });
  }
  const DWORD data_attributes =
      GetFileAttributesW(verified_data_directory.c_str());
  if (data_attributes == INVALID_FILE_ATTRIBUTES) {
    return ytec::clonecore::Result<ProductLogSession>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"書込み確認済みdataフォルダーの再確認",
            GetLastError()));
  }
  if ((data_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (data_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::invalid_data,
        .native_code = ERROR_REPARSE_TAG_INVALID,
        .operation = L"書込み確認済みdataフォルダーの再確認",
        .message =
            L"通常の非reparseフォルダー以外には診断ログを作成しません",
    });
  }

  const std::wstring log_directory =
      verified_data_directory + L"\\logs";
  if (CreateDirectoryW(log_directory.c_str(), nullptr) == FALSE) {
    const DWORD create_error = GetLastError();
    if (create_error != ERROR_ALREADY_EXISTS) {
      return ytec::clonecore::Result<ProductLogSession>::failure(
          ytec::clonecore::make_win32_error(
              ytec::clonecore::ErrorCode::io_failed,
              L"診断ログフォルダー作成",
              create_error));
    }
  }
  const DWORD log_attributes = GetFileAttributesW(log_directory.c_str());
  if (log_attributes == INVALID_FILE_ATTRIBUTES) {
    return ytec::clonecore::Result<ProductLogSession>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"診断ログフォルダー確認",
            GetLastError()));
  }
  if ((log_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (log_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::invalid_data,
        .native_code = ERROR_REPARSE_TAG_INVALID,
        .operation = L"診断ログフォルダー確認",
        .message =
            L"通常のローカルフォルダー以外には診断ログを作成しません",
    });
  }

  auto retention =
      ytec::windowsapp::enforce_windows_product_log_retention(
          verified_data_directory);
  if (!retention) {
    return ytec::clonecore::Result<ProductLogSession>::failure(
        retention.error());
  }

  SYSTEMTIME local_time{};
  GetLocalTime(&local_time);
  std::array<wchar_t, 96U> base_name{};
  const int base_length = swprintf_s(
      base_name.data(),
      base_name.size(),
      L"TsumugiDrive-failed-%04u%02u%02u-%02u%02u%02u-%03u-%lu",
      static_cast<unsigned int>(local_time.wYear),
      static_cast<unsigned int>(local_time.wMonth),
      static_cast<unsigned int>(local_time.wDay),
      static_cast<unsigned int>(local_time.wHour),
      static_cast<unsigned int>(local_time.wMinute),
      static_cast<unsigned int>(local_time.wSecond),
      static_cast<unsigned int>(local_time.wMilliseconds),
      static_cast<unsigned long>(GetCurrentProcessId()));
  if (base_length <= 0) {
    return ytec::clonecore::Result<ProductLogSession>::failure({
        .code = ytec::clonecore::ErrorCode::internal_error,
        .native_code = ERROR_INVALID_DATA,
        .operation = L"診断ログ名作成",
        .message = L"診断ログ名を作成できません",
    });
  }

  for (unsigned int attempt = 0U; attempt < 10U; ++attempt) {
    std::wstring file_name(
        base_name.data(), static_cast<std::size_t>(base_length));
    if (attempt != 0U) {
      file_name += L"-" + std::to_wstring(attempt);
    }
    const std::wstring log_path =
        log_directory + L"\\" + file_name + L".log";
    auto logger = ytec::clonecore::make_utf8_file_logger(
        log_path, true);
    if (logger) {
      auto error_detected = std::make_shared<std::atomic_bool>(false);
      ytec::clonecore::Logger monitored_logger(
          [sink = logger.take_value(), error_detected](
              const ytec::clonecore::LogRecord& record) noexcept {
            if (record.level == ytec::clonecore::LogLevel::error) {
              error_detected->store(true);
            }
            sink.write(record.level, record.message);
          });
      return ytec::clonecore::Result<ProductLogSession>::success({
          .logger = std::move(monitored_logger),
          .path = log_path,
          .error_detected = std::move(error_detected),
          .retention_deleted_count = retention.value().deleted_count,
          .retention_deleted_bytes = retention.value().deleted_bytes,
      });
    }
    if (logger.error().native_code != ERROR_FILE_EXISTS &&
        logger.error().native_code != ERROR_ALREADY_EXISTS) {
      return ytec::clonecore::Result<ProductLogSession>::failure(
          logger.error());
    }
  }
  return ytec::clonecore::Result<ProductLogSession>::failure({
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_FILE_EXISTS,
      .operation = L"診断ログファイル作成",
      .message = L"重複しない新しい診断ログ名を確保できません",
  });
}

struct SourceSafeLogBackingProof final {
  ytec::windowsapp::StartupDataBackingObservation observation;
  ytec::clonecore::StableDiskIdentity backing_identity;
  ytec::windowsapp::StartupDataBackingRelationship relationship{
      ytec::windowsapp::StartupDataBackingRelationship::unknown};
};

ytec::clonecore::Error source_safe_log_observation_error(
    const std::wstring_view message,
    const DWORD native_code = ERROR_DEVICE_NOT_CONNECTED) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::identity_mismatch,
      .native_code = native_code,
      .operation = L"診断ログ保存先のコピー元／コピー先分離確認",
      .message = std::wstring(message),
  };
}

ytec::clonecore::Result<ytec::clonecore::StableDiskIdentity>
find_unique_current_disk_identity(
    const ytec::clonecore::StableDiskIdentity& expected,
    const ytec::diskmodel::InventoryReport& inventory) {
  std::optional<ytec::clonecore::StableDiskIdentity> match;
  for (const auto& disk : inventory.disks) {
    auto observed = ytec::diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!observed ||
        !ytec::clonecore::validate_stable_identity(
            expected, observed.value(), L"ログ保護対象ディスク")) {
      continue;
    }
    if (match.has_value()) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(
          source_safe_log_observation_error(
              L"保護対象の安定識別情報が複数ディスクへ一致しました"));
    }
    match = observed.take_value();
  }
  if (!match.has_value()) {
    return ytec::clonecore::Result<
        ytec::clonecore::StableDiskIdentity>::failure(
        source_safe_log_observation_error(
            L"保護対象ディスクを現在の接続状態で再識別できません"));
  }
  return ytec::clonecore::Result<
      ytec::clonecore::StableDiskIdentity>::success(
      std::move(match.value()));
}

ytec::clonecore::Result<SourceSafeLogBackingProof>
prove_source_safe_log_backing(
    const std::span<const ytec::clonecore::StableDiskIdentity>
        protected_identities,
    const std::optional<ytec::clonecore::StableDiskIdentity>&
        expected_persistent_backing = std::nullopt) {
  auto before =
      ytec::windowsapp::inspect_windows_startup_data_backing();
  if (!before) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        before.error());
  }
  auto inventory_provider =
      ytec::diskmodel::make_windows_disk_inventory_provider(nullptr);
  auto inventory = inventory_provider->enumerate();
  if (!inventory) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        inventory.error());
  }
  auto after =
      ytec::windowsapp::inspect_windows_startup_data_backing();
  if (!after) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        after.error());
  }
  if (before.value().disk_number != after.value().disk_number ||
      before.value().application_directory !=
          after.value().application_directory ||
      before.value().data_directory != after.value().data_directory ||
      before.value().data_directory_exists !=
          after.value().data_directory_exists) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        source_safe_log_observation_error(
            L"読取り専用確認中にEXE／dataの保存先が変化しました"));
  }

  const auto backing_disk = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [&](const auto& disk) {
        return disk.disk_number == before.value().disk_number;
      });
  if (backing_disk == inventory.value().disks.end()) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        source_safe_log_observation_error(
            L"EXE／dataの物理ディスクを現在の列挙結果で確認できません"));
  }
  auto backing_identity = ytec::diskmodel::make_stable_disk_identity(
      *backing_disk, backing_disk->is_system_disk);
  if (!backing_identity) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        backing_identity.error());
  }
  if (expected_persistent_backing.has_value() &&
      !ytec::clonecore::validate_stable_identity(
          expected_persistent_backing.value(),
          backing_identity.value(),
          L"永続診断ログの保存ディスク")) {
    return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
        source_safe_log_observation_error(
            L"永続診断ログの保存ディスクが接続し直し等で変化しました"));
  }

  std::vector<std::uint32_t> protected_disk_numbers;
  protected_disk_numbers.reserve(protected_identities.size());
  for (const auto& expected : protected_identities) {
    auto current = find_unique_current_disk_identity(
        expected, inventory.value());
    if (!current) {
      return ytec::clonecore::Result<SourceSafeLogBackingProof>::failure(
          current.error());
    }
    protected_disk_numbers.push_back(current.value().disk_number);
  }
  return ytec::clonecore::Result<SourceSafeLogBackingProof>::success({
      .observation = before.take_value(),
      .backing_identity = backing_identity.take_value(),
      .relationship =
          ytec::windowsapp::classify_startup_data_backing(
              protected_disk_numbers.empty()
                  ? std::optional<std::uint32_t>{}
                  : std::optional<std::uint32_t>{
                        backing_disk->disk_number},
              protected_disk_numbers),
  });
}

ytec::clonecore::Result<ytec::clonecore::StableDiskIdentity>
identify_local_path_backing_read_only(const std::wstring& path) {
  auto before =
      ytec::diskmodel::query_single_disk_number_for_local_path(path);
  if (!before) {
    return ytec::clonecore::Result<
        ytec::clonecore::StableDiskIdentity>::failure(before.error());
  }
  auto inventory_provider =
      ytec::diskmodel::make_windows_disk_inventory_provider(nullptr);
  auto inventory = inventory_provider->enumerate();
  if (!inventory) {
    return ytec::clonecore::Result<
        ytec::clonecore::StableDiskIdentity>::failure(inventory.error());
  }
  auto after =
      ytec::diskmodel::query_single_disk_number_for_local_path(path);
  if (!after || before.value() != after.value()) {
    return ytec::clonecore::Result<
        ytec::clonecore::StableDiskIdentity>::failure(
        source_safe_log_observation_error(
            L"ローカル保存先の物理ディスクが読取り専用確認中に変化しました"));
  }
  const auto disk = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [&](const auto& item) {
        return item.disk_number == before.value();
      });
  if (disk == inventory.value().disks.end()) {
    return ytec::clonecore::Result<
        ytec::clonecore::StableDiskIdentity>::failure(
        source_safe_log_observation_error(
            L"ローカル保存先を現在のディスク列挙で再識別できません"));
  }
  return ytec::diskmodel::make_stable_disk_identity(
      *disk, disk->is_system_disk);
}

void isolate_product_log_to_ram(
    AppState& state,
    const std::wstring& reason) {
  state.log_router.isolate_to_ram_permanently();
  state.log_path.clear();
  state.persistent_log_error_detected.reset();
  state.persistent_log_backing_identity.reset();
  state.startup_data_policy =
      ytec::windowsapp::make_read_only_bootstrap_data_policy();
  state.startup_data_policy.diagnostic = reason;
  if (state.log_error.empty()) {
    state.log_error = reason;
  }
  if (state.logger.has_value()) {
    state.logger->warning(
        L"診断ログをこのセッション中はbounded RAMへ不可逆隔離 reason=" +
        reason);
  }
}

bool prepare_source_safe_product_logging(
    AppState& state,
    const std::span<const ytec::clonecore::StableDiskIdentity>
        protected_identities,
    const std::wstring_view operation) {
  if (state.log_router.permanently_ram_only()) {
    state.startup_data_policy =
        ytec::windowsapp::make_read_only_bootstrap_data_policy();
    return true;
  }

  auto proof = prove_source_safe_log_backing(
      protected_identities,
      state.persistent_log_backing_identity);
  if (!proof ||
      proof.value().relationship !=
          ytec::windowsapp::StartupDataBackingRelationship::disjoint) {
    const std::wstring reason = !proof
        ? L"EXE／dataの物理ディスクを全保護対象から分離して証明できません: " +
              format_error_message(proof.error())
        : proof.value().relationship ==
                  ytec::windowsapp::StartupDataBackingRelationship::
                      protected_disk
              ? L"EXE／dataが操作のコピー元またはコピー先と同じ物理ディスクです"
              : L"操作のコピー元／コピー先をすべて安定識別できないため永続ログを使用しません";
    isolate_product_log_to_ram(state, reason);
    return true;
  }

  if (state.log_router.persistent_sink_attached()) {
    state.startup_data_policy.storage =
        ytec::windowsapp::StartupDataStorage::persistent_data;
    state.startup_data_policy.issue =
        ytec::windowsapp::StartupDataIssue::none;
    state.startup_data_policy.data_directory =
        proof.value().observation.data_directory;
    state.startup_data_policy.diagnostic.clear();
    state.startup_data_policy.native_code = ERROR_SUCCESS;
    return true;
  }

  auto persistent_policy =
      ytec::windowsapp::inspect_windows_startup_data_policy();
  if (!persistent_policy.persistent_logging_permitted()) {
    state.startup_data_policy = std::move(persistent_policy);
    state.log_error = diagnostic_only_message(state);
    if (state.logger.has_value()) {
      state.logger->error(
          std::wstring(operation) +
          L"のon-demand data書込み確認に失敗");
    }
    return false;
  }

  auto after_probe = prove_source_safe_log_backing(
      protected_identities, proof.value().backing_identity);
  if (!after_probe ||
      after_probe.value().relationship !=
          ytec::windowsapp::StartupDataBackingRelationship::disjoint) {
    isolate_product_log_to_ram(
        state,
        L"data書込み確認後に保存ディスクの分離を再証明できません");
    return true;
  }

  auto session = create_product_log_session(
      persistent_policy.data_directory);
  if (!session) {
    state.startup_data_policy.storage =
        ytec::windowsapp::StartupDataStorage::unavailable;
    state.startup_data_policy.issue =
        ytec::windowsapp::StartupDataIssue::write_probe_failed;
    state.startup_data_policy.data_directory =
        persistent_policy.data_directory;
    state.startup_data_policy.diagnostic =
        format_error_message(session.error());
    state.startup_data_policy.native_code =
        session.error().native_code;
    state.log_error = diagnostic_only_message(state);
    if (state.logger.has_value()) {
      state.logger->error(
          std::wstring(operation) + L"の永続診断ログ作成に失敗");
    }
    return false;
  }

  auto after_creation = prove_source_safe_log_backing(
      protected_identities, after_probe.value().backing_identity);
  if (!after_creation ||
      after_creation.value().relationship !=
          ytec::windowsapp::StartupDataBackingRelationship::disjoint) {
    isolate_product_log_to_ram(
        state,
        L"ログファイル作成直後に保存ディスクの分離を再証明できません");
    return true;
  }

  auto product_session = session.take_value();
  if (!state.log_router.attach_persistent_sink(
          std::move(product_session.logger))) {
    isolate_product_log_to_ram(
        state, L"永続診断ログをRAMルーターへ安全に接続できません");
    return true;
  }
  state.log_path = std::move(product_session.path);
  state.persistent_log_error_detected =
      std::move(product_session.error_detected);
  state.log_error.clear();
  state.persistent_log_backing_identity =
      after_creation.value().backing_identity;
  state.startup_data_policy = std::move(persistent_policy);
  if (state.logger.has_value()) {
    state.logger->info(
        L"ログ循環完了 deleted_count=" +
        std::to_wstring(product_session.retention_deleted_count) +
        L" deleted_bytes=" +
        std::to_wstring(product_session.retention_deleted_bytes));
    state.logger->info(
        std::wstring(operation) +
        L"の保護対象と分離確認後に永続診断ログを開始");
  }
  return true;
}

void complete_product_log_on_shutdown(
    AppState& state,
    const bool active_write_operation) noexcept {
  try {
    const std::wstring failed_path = state.log_path;
    const std::wstring data_directory =
        state.startup_data_policy.data_directory;
    const bool error_detected =
        state.persistent_log_error_detected == nullptr ||
        state.persistent_log_error_detected->load();

    // This is the owning transition that closes the persistent file handle.
    // Existing worker Logger copies route only to the bounded RAM state after
    // this point, so the failed name can be considered for handle rename.
    state.log_router.isolate_to_ram_permanently();
    state.persistent_log_error_detected.reset();
    state.persistent_log_backing_identity.reset();
    if (failed_path.empty() || data_directory.empty()) {
      state.log_path.clear();
      return;
    }

    const auto completion =
        ytec::windowsapp::complete_windows_product_log_session(
            data_directory,
            failed_path,
            state.clean_close_requested,
            error_detected,
            active_write_operation);
    if (completion && completion.value().promoted) {
      state.log_path =
          (std::filesystem::path(data_directory) / L"logs" /
           completion.value().plan.normal_file_name)
              .wstring();
    } else {
      // Every rejection, error, collision, or rename failure retains the
      // already-created failed name. No fallback path or overwrite is used.
      state.log_path = failed_path;
    }
  } catch (...) {
    state.log_router.isolate_to_ram_permanently();
    state.persistent_log_error_detected.reset();
    state.persistent_log_backing_identity.reset();
    // The on-disk name was failed-first, so an unexpected finalization error
    // is already represented safely without another filesystem operation.
  }
}

void log_error_summary(
    const std::optional<ytec::clonecore::Logger>& logger,
    const std::wstring_view context,
    const ytec::clonecore::Error& error) noexcept {
  if (!logger.has_value()) {
    return;
  }
  logger->error(
      std::wstring(context) + L" code=" +
      std::wstring(ytec::clonecore::error_code_name(error.code)) +
      L" native_code=" + std::to_wstring(error.native_code));
}

[[nodiscard]] int completion_power_radio_id(
    const ytec::clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case ytec::clonecore::CompletionPowerAction::none:
      return kCompletionPowerNoneId;
    case ytec::clonecore::CompletionPowerAction::sleep:
      return kCompletionPowerSleepId;
    case ytec::clonecore::CompletionPowerAction::restart:
      return kCompletionPowerRestartId;
    case ytec::clonecore::CompletionPowerAction::shutdown:
      return kCompletionPowerShutdownId;
  }
  return 0;
}

[[nodiscard]] ytec::clonecore::CompletionPowerAction
completion_power_action_from_radio_id(const int radio_id) noexcept {
  switch (radio_id) {
    case kCompletionPowerSleepId:
      return ytec::clonecore::CompletionPowerAction::sleep;
    case kCompletionPowerRestartId:
      return ytec::clonecore::CompletionPowerAction::restart;
    case kCompletionPowerShutdownId:
      return ytec::clonecore::CompletionPowerAction::shutdown;
    case kCompletionPowerNoneId:
    default:
      return ytec::clonecore::CompletionPowerAction::none;
  }
}

[[nodiscard]] std::wstring_view completion_power_action_label(
    const ytec::clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case ytec::clonecore::CompletionPowerAction::none:
      return L"何もしない";
    case ytec::clonecore::CompletionPowerAction::sleep:
      return L"スリープ";
    case ytec::clonecore::CompletionPowerAction::restart:
      return L"再起動";
    case ytec::clonecore::CompletionPowerAction::shutdown:
      return L"シャットダウン";
  }
  return L"何もしない";
}

// Returns true only when an accepted restart/shutdown is expected to end this
// UI session. Sleep returns false after resume so callers still refresh their
// inventory. Merely displaying or cancelling either dialog also returns false.
[[nodiscard]] bool offer_completion_power_action(
    AppState& state,
    const ytec::windowsapp::WindowsCompletionPowerProof& proof,
    const std::wstring_view completed_operation) {
  const auto prompt_plan =
      ytec::windowsapp::plan_windows_completion_power_prompt(proof);
  if (!prompt_plan.prompt_allowed) {
    return false;
  }

  auto platform =
      ytec::clonecore::make_windows_completion_power_platform();
  if (platform == nullptr) {
    if (state.logger.has_value()) {
      state.logger->warning(
          L"完了後動作を提示しません platform_factory=null");
    }
    return false;
  }
  const auto availability =
      ytec::clonecore::query_completion_power_availability(
          ytec::clonecore::CompletionPowerEnvironment::windows,
          *platform);
  const auto actions =
      ytec::clonecore::available_completion_power_actions(availability);
  constexpr std::array<std::wstring_view, 4U> kLabels{
      L"何もしない（既定）",
      L"スリープ",
      L"再起動",
      L"シャットダウン",
  };
  std::vector<TASKDIALOG_BUTTON> radio_buttons;
  radio_buttons.reserve(actions.size());
  for (const auto action : actions) {
    const auto index = static_cast<std::size_t>(action);
    if (index >= kLabels.size()) {
      continue;
    }
    radio_buttons.push_back(TASKDIALOG_BUTTON{
        completion_power_radio_id(action),
        kLabels[index].data(),
    });
  }
  if (radio_buttons.empty() ||
      radio_buttons.front().nButtonID != kCompletionPowerNoneId) {
    return false;
  }

  const HWND previous_focus = GetFocus();
  const std::wstring instruction =
      std::wstring(completed_operation) + L"が安全に完了しました";
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = state.window;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_SIZE_TO_CONTENT;
  config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = L"完了後の動作";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction = instruction.c_str();
  config.pszContent =
      L"必須検証と自動スリープ防止の解除を確認しました。"
      L"このあとWindowsへ要求する動作を選んでください。";
  config.cRadioButtons = static_cast<UINT>(radio_buttons.size());
  config.pRadioButtons = radio_buttons.data();
  config.nDefaultRadioButton = kCompletionPowerNoneId;
  config.nDefaultButton = IDOK;
  config.pszFooterIcon = TD_INFORMATION_ICON;
  config.pszFooter =
      availability.sleep ==
              ytec::clonecore::CompletionPowerAvailabilityState::available
          ? L"既定は「何もしない」です。電源操作は次の画面でもう一度確認します。"
          : L"既定は「何もしない」です。このPCではスリープを選べません。";

  using TaskDialogIndirectFunction = HRESULT(WINAPI*)(
      const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE common_controls = LoadLibraryExW(
      L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  TaskDialogIndirectFunction task_dialog{};
  if (common_controls != nullptr) {
    task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
        GetProcAddress(common_controls, "TaskDialogIndirect"));
    if (task_dialog == nullptr) {
      task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
          GetProcAddress(common_controls, MAKEINTRESOURCEA(345)));
    }
  }
  int pressed_button{};
  int selected_radio = kCompletionPowerNoneId;
  const HRESULT shown = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(
            &config, &pressed_button, &selected_radio, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(shown) || pressed_button != IDOK) {
    if (FAILED(shown) && state.logger.has_value()) {
      state.logger->warning(
          L"完了後動作の選択画面を表示できないため何もしません");
    }
    restore_error_dialog_focus(previous_focus);
    return false;
  }

  const auto selected_action =
      completion_power_action_from_radio_id(selected_radio);
  if (selected_action ==
      ytec::clonecore::kDefaultCompletionPowerAction) {
    if (state.logger.has_value()) {
      state.logger->info(L"完了後動作 selection=none");
    }
    restore_error_dialog_focus(previous_focus);
    return false;
  }

  const std::wstring confirmation =
      L"「" + std::wstring(completion_power_action_label(selected_action)) +
      L"」を今すぐWindowsへ要求します。\r\n\r\n"
      L"保存していない作業がある場合は［いいえ］を選んでください。";
  const int confirmed = MessageBoxW(
      state.window,
      confirmation.c_str(),
      L"完了後の電源操作を再確認",
      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (confirmed != IDYES) {
    if (state.logger.has_value()) {
      state.logger->info(L"完了後動作 reconfirmed=false");
    }
    restore_error_dialog_focus(previous_focus);
    return false;
  }

  const auto request = ytec::windowsapp::
      make_windows_completion_power_execution_request(
          proof, selected_action, true, true);
  const auto result = ytec::clonecore::execute_completion_power_action(
      request, *platform);
  if (result.disposition == ytec::clonecore::
          CompletionPowerExecutionDisposition::request_accepted) {
    if (state.logger.has_value()) {
      state.logger->info(
          L"完了後動作 request_accepted action=" +
          std::wstring(completion_power_action_label(selected_action)));
    }
    const bool session_end_expected = ytec::windowsapp::
        completion_power_action_expects_ui_session_end(selected_action);
    if (!session_end_expected) {
      restore_error_dialog_focus(previous_focus);
      InvalidateRect(state.window, nullptr, TRUE);
    }
    return session_end_expected;
  }
  if (result.error.has_value()) {
    log_error_summary(
        state.logger, L"完了後動作を実行せず停止", result.error.value());
    show_product_error(
        state.window,
        L"完了後の電源操作を実行できませんでした",
        result.error.value(),
        L"元のクローン／イメージ／メディア処理の完了結果は変わりません。Windowsはこのまま使用できます。");
  }
  restore_error_dialog_focus(previous_focus);
  return false;
}

void log_inventory_summary(
    const std::optional<ytec::clonecore::Logger>& logger,
    const ytec::diskmodel::InventoryReport& report) noexcept {
  if (!logger.has_value()) {
    return;
  }
  logger->info(
      L"読み取り専用ディスク列挙完了 disks=" +
      std::to_wstring(report.disks.size()) + L" issues=" +
      std::to_wstring(report.issues.size()));
  for (const auto& disk : report.disks) {
    logger->info(
        L"ディスク要約 number=" +
        std::to_wstring(disk.disk_number) + L" model=" +
        (disk.model.empty() ? L"不明" : disk.model) + L" bytes=" +
        std::to_wstring(disk.size_bytes) + L" logical_sector=" +
        std::to_wstring(disk.logical_sector_size) +
        L" physical_sector=" +
        std::to_wstring(disk.physical_sector_size) + L" bus=" +
        (disk.bus_type.empty() ? L"不明" : disk.bus_type) +
        L" disk_id=" +
        (disk.serial_log_token.empty() ? L"不明" : disk.serial_log_token) +
        L" style=" + partition_style_text(disk.partition_style) +
        L" health=" + std::wstring(
            ytec::diskmodel::disk_health_state_name(disk.health.state)) +
        L" temperature_celsius=" +
        (disk.health.temperature_celsius.has_value()
             ? std::to_wstring(disk.health.temperature_celsius.value())
             : L"unknown") +
        L" temperature_warning=" +
        (disk.health.temperature_warning ? L"true" : L"false") +
        L" partitions=" +
        std::to_wstring(disk.partitions.size()) + L" system=" +
        (disk.is_system_disk ? L"true" : L"false"));
  }
  for (const auto& issue : report.issues) {
    logger->warning(
        L"ディスク列挙項目の警告 code=" +
        std::wstring(
            ytec::clonecore::error_code_name(issue.error.code)) +
        L" native_code=" +
        std::to_wstring(issue.error.native_code));
  }
}

INT_PTR CALLBACK confirmation_dialog_proc(
    const HWND dialog,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<ConfirmationDialogState*>(
      GetWindowLongPtrW(dialog, GWLP_USERDATA));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<ConfirmationDialogState*>(lparam);
    SetWindowLongPtrW(
        dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    if (state->font != nullptr) {
      SendMessageW(
          dialog,
          WM_SETFONT,
          reinterpret_cast<WPARAM>(state->font),
          TRUE);
      EnumChildWindows(
          dialog,
          [](const HWND child, const LPARAM font) -> BOOL {
            SendMessageW(
                child,
                WM_SETFONT,
                static_cast<WPARAM>(font),
                TRUE);
            return TRUE;
          },
          reinterpret_cast<LPARAM>(state->font));
    }
    SetDlgItemTextW(dialog, IDC_CONFIRM_DETAILS, state->details.c_str());
    const std::wstring token_text =
        L"入力する確認語:\r\n" + state->token;
    SetDlgItemTextW(dialog, IDC_CONFIRM_TOKEN, token_text.c_str());
    SetDlgItemTextW(
        dialog,
        IDOK,
        state->confirm_button_label.c_str());
    SendDlgItemMessageW(
        dialog, IDC_CONFIRM_EDIT, EM_SETLIMITTEXT, 1024, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
    return TRUE;
  }
  if (message == WM_COMMAND && state != nullptr) {
    const int identifier = LOWORD(wparam);
    if (identifier == IDC_CONFIRM_EDIT &&
        HIWORD(wparam) == EN_CHANGE) {
      const int length =
          GetWindowTextLengthW(GetDlgItem(dialog, IDC_CONFIRM_EDIT));
      std::vector<wchar_t> text(
          static_cast<std::size_t>((std::max)(length, 0)) + 1,
          L'\0');
      GetDlgItemTextW(
          dialog,
          IDC_CONFIRM_EDIT,
          text.data(),
          static_cast<int>(text.size()));
      EnableWindow(
          GetDlgItem(dialog, IDOK),
          std::wstring_view(text.data()) == state->token ? TRUE : FALSE);
      return TRUE;
    }
    if (identifier == IDOK) {
      if (IsWindowEnabled(GetDlgItem(dialog, IDOK)) != FALSE) {
        EndDialog(dialog, IDOK);
      }
      return TRUE;
    }
    if (identifier == IDCANCEL) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  return FALSE;
}

bool read_ascii_password_control(
    const HWND dialog,
    const int identifier,
    std::string& destination) {
  erase_secret(destination);
  const HWND control = GetDlgItem(dialog, identifier);
  const int length = GetWindowTextLengthW(control);
  if (length < 0 || length > 1024) {
    return false;
  }
  std::vector<wchar_t> wide(
      static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied = GetWindowTextW(
      control, wide.data(), static_cast<int>(wide.size()));
  if (copied != length) {
    SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
    return false;
  }
  destination.reserve(static_cast<std::size_t>(length));
  bool valid = true;
  for (int index = 0; index < length; ++index) {
    const wchar_t character = wide[static_cast<std::size_t>(index)];
    if (character < 0x20 || character > 0x7E) {
      valid = false;
      break;
    }
    destination.push_back(static_cast<char>(character));
  }
  SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
  if (!valid) {
    erase_secret(destination);
  }
  return valid;
}

INT_PTR CALLBACK tsumugi_password_dialog_proc(
    const HWND dialog,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<TsumugiPasswordDialogState*>(
      GetWindowLongPtrW(dialog, GWLP_USERDATA));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<TsumugiPasswordDialogState*>(lparam);
    SetWindowLongPtrW(
        dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    SetWindowTextW(dialog, state->title.c_str());
    SetDlgItemTextW(dialog, IDC_PASSWORD_PROMPT, state->prompt.c_str());
    SetDlgItemTextW(
        dialog, IDOK, state->confirm_button_label.c_str());
    SendDlgItemMessageW(
        dialog, IDC_PASSWORD_EDIT, EM_SETLIMITTEXT, 1024, 0);
    SendDlgItemMessageW(
        dialog, IDC_PASSWORD_CONFIRM_EDIT, EM_SETLIMITTEXT, 1024, 0);
    if (!state->require_confirmation) {
      ShowWindow(
          GetDlgItem(dialog, IDC_PASSWORD_CONFIRM_LABEL), SW_HIDE);
      ShowWindow(
          GetDlgItem(dialog, IDC_PASSWORD_CONFIRM_EDIT), SW_HIDE);
      SetDlgItemTextW(
          dialog,
          IDC_PASSWORD_NOTE,
          state->allow_empty
              ? L"暗号化していない場合は空欄のまま続行できます。パスワードは保存しません。"
              : L"8文字以上のASCII印字文字。パスワードは保存しません。");
    }
    if (state->font != nullptr) {
      SendMessageW(
          dialog,
          WM_SETFONT,
          reinterpret_cast<WPARAM>(state->font),
          TRUE);
      EnumChildWindows(
          dialog,
          [](const HWND child, const LPARAM font) -> BOOL {
            SendMessageW(
                child,
                WM_SETFONT,
                static_cast<WPARAM>(font),
                TRUE);
            return TRUE;
          },
          reinterpret_cast<LPARAM>(state->font));
    }
    SetFocus(GetDlgItem(dialog, IDC_PASSWORD_EDIT));
    return FALSE;
  }
  if (message == WM_COMMAND && state != nullptr) {
    const int identifier = LOWORD(wparam);
    if (identifier == IDOK) {
      std::string password;
      if (!read_ascii_password_control(
              dialog, IDC_PASSWORD_EDIT, password)) {
        MessageBoxW(
            dialog,
            L"ASCII印字文字（半角英数字・記号）だけを入力してください。",
            L"パスワードを確認してください",
            MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      if (password.empty() && state->allow_empty) {
        erase_secret(state->password);
        SetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, L"");
        EndDialog(dialog, IDOK);
        return TRUE;
      }
      const auto assessment =
          ytec::imageformat::assess_tsumugi_password(password);
      if (!assessment.accepted) {
        erase_secret(password);
        MessageBoxW(
            dialog,
            L"8文字以上のASCII印字文字で入力してください。",
            L"パスワードを確認してください",
            MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      if (state->require_confirmation) {
        std::string confirmation;
        const bool confirmation_valid = read_ascii_password_control(
            dialog, IDC_PASSWORD_CONFIRM_EDIT, confirmation);
        const bool matches =
            confirmation_valid && confirmation == password;
        erase_secret(confirmation);
        if (!matches) {
          erase_secret(password);
          MessageBoxW(
              dialog,
              L"確認用パスワードが一致しません。",
              L"パスワードを確認してください",
              MB_OK | MB_ICONWARNING);
          return TRUE;
        }
      }
      if (assessment.weak &&
          MessageBoxW(
              dialog,
              L"このパスワードは短いか、文字種が少ないため推測されやすい可能性があります。\n\n回復キーはなく、紛失した場合は復元できません。弱いパスワードのまま続けますか？",
              L"弱いパスワードの確認",
              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        erase_secret(password);
        return TRUE;
      }
      erase_secret(state->password);
      state->password.assign(password);
      erase_secret(password);
      SetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, L"");
      SetDlgItemTextW(dialog, IDC_PASSWORD_CONFIRM_EDIT, L"");
      EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (identifier == IDCANCEL) {
      SetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, L"");
      SetDlgItemTextW(dialog, IDC_PASSWORD_CONFIRM_EDIT, L"");
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  if (message == WM_CLOSE) {
    SetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, L"");
    SetDlgItemTextW(dialog, IDC_PASSWORD_CONFIRM_EDIT, L"");
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}

TsumugiPasswordPromptResult prompt_tsumugi_password(
    const HWND owner,
    const HFONT font,
    std::wstring prompt,
    std::wstring title,
    const bool require_confirmation,
    const bool allow_empty,
    std::wstring confirm_button_label) {
  TsumugiPasswordDialogState state{
      .prompt = std::move(prompt),
      .title = std::move(title),
      .confirm_button_label = std::move(confirm_button_label),
      .font = font,
      .require_confirmation = require_confirmation,
      .allow_empty = allow_empty,
  };
  const INT_PTR result = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_TSUMUGI_PASSWORD),
      owner,
      tsumugi_password_dialog_proc,
      reinterpret_cast<LPARAM>(&state));
  if (result != IDOK) {
    if (result == -1) {
      MessageBoxW(
          owner,
          L"パスワード入力画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return {};
  }
  TsumugiPasswordPromptResult prompt_result{
      .accepted = true,
  };
  if (!state.password.empty()) {
    prompt_result.password =
        std::make_shared<SecureAsciiPassword>(state.password);
  }
  return prompt_result;
}

bool process_is_elevated() {
  HANDLE token{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
    return false;
  }
  TOKEN_ELEVATION elevation{};
  DWORD returned{};
  const BOOL result = GetTokenInformation(
      token,
      TokenElevation,
      &elevation,
      static_cast<DWORD>(sizeof(elevation)),
      &returned);
  CloseHandle(token);
  return result != FALSE && elevation.TokenIsElevated != 0;
}

void set_control_font(const HWND control, const HFONT font) {
  SendMessageW(
      control,
      WM_SETFONT,
      reinterpret_cast<WPARAM>(font),
      static_cast<LPARAM>(TRUE));
}

void draw_text(
    HDC dc,
    const std::wstring_view text,
    RECT area,
    const COLORREF color,
    const UINT format,
    const HFONT font) {
  const auto previous_font =
      static_cast<HFONT>(SelectObject(dc, font));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(
      dc,
      text.data(),
      static_cast<int>(text.size()),
      &area,
      format);
  SelectObject(dc, previous_font);
}

void fill_rounded_rect(
    HDC dc,
    const RECT area,
    const COLORREF fill,
    const COLORREF border,
    const int radius = 14) {
  const HBRUSH brush = CreateSolidBrush(fill);
  const HPEN pen = CreatePen(PS_SOLID, 1, border);
  const auto old_brush = SelectObject(dc, brush);
  const auto old_pen = SelectObject(dc, pen);
  RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);
}

void draw_progress_metric(
    const AppState& state,
    HDC dc,
    const RECT& area,
    const std::wstring_view label,
    const std::wstring_view value,
    const COLORREF accent,
    const COLORREF fill) {
  fill_rounded_rect(dc, area, fill, accent, 10);
  RECT accent_bar = area;
  accent_bar.right = accent_bar.left + 4;
  const HBRUSH accent_brush = CreateSolidBrush(accent);
  FillRect(dc, &accent_bar, accent_brush);
  DeleteObject(accent_brush);
  RECT label_area{
      area.left + 12,
      area.top + 7,
      area.right - 8,
      area.top + 27};
  draw_text(
      dc,
      label,
      label_area,
      accent,
      DT_LEFT | DT_SINGLELINE | DT_TOP,
      state.small_font);
  RECT value_area{
      area.left + 12,
      area.top + 29,
      area.right - 8,
      area.bottom - 5};
  draw_text(
      dc,
      value,
      value_area,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS,
      state.small_font);
}

void draw_strands(HDC dc, const int left, const int top, const int width) {
  const HPEN blue = CreatePen(PS_SOLID, 3, kTsumugiBlue);
  const HPEN purple = CreatePen(PS_SOLID, 3, kTsumugiPurple);
  const HPEN green = CreatePen(PS_SOLID, 3, kSafeGreen);
  const auto previous = SelectObject(dc, blue);
  MoveToEx(dc, left, top + 8, nullptr);
  for (int x = 0; x <= width; x += 8) {
    const int y = top + 8 + ((x / 8) % 4 < 2 ? -3 : 3);
    LineTo(dc, left + x, y);
  }
  SelectObject(dc, purple);
  MoveToEx(dc, left, top + 14, nullptr);
  for (int x = 0; x <= width; x += 8) {
    const int y = top + 14 + ((x / 8) % 4 < 2 ? 3 : -3);
    LineTo(dc, left + x, y);
  }
  SelectObject(dc, green);
  MoveToEx(dc, left, top + 20, nullptr);
  for (int x = 0; x <= width; x += 8) {
    const int y = top + 20 + ((x / 8) % 4 < 2 ? -2 : 2);
    LineTo(dc, left + x, y);
  }
  SelectObject(dc, previous);
  DeleteObject(green);
  DeleteObject(purple);
  DeleteObject(blue);
}

void layout_controls(AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  const bool compact_height = client.bottom < 680;
  const int nav_width = 250;
  const int nav_left = 22;
  const int nav_button_width = nav_width - 44;
  const int nav_button_height = compact_height ? 40 : 44;
  const int nav_button_step = compact_height ? 46 : 52;
  int nav_top = compact_height ? 150 : 156;
  for (const HWND button : state.navigation) {
    MoveWindow(
        button,
        nav_left,
        nav_top,
        nav_button_width,
        nav_button_height,
        TRUE);
    nav_top += nav_button_step;
  }

  const auto clone_layout =
      ytec::windowsapp::calculate_clone_column_layout(client.right);
  const auto bottom_action_layout =
      ytec::windowsapp::calculate_bottom_action_layout(client.right);
  const auto image_option_layout =
      ytec::windowsapp::calculate_image_create_option_layout(client.right);
  MoveWindow(state.refresh, client.right - 152, 34, 116, 36, TRUE);
  MoveWindow(
      state.transfer_mode_combo,
      image_option_layout.transfer_control.left,
      76,
      image_option_layout.transfer_control.width(),
      160,
      TRUE);
  MoveWindow(
      state.image_verification_mode_combo,
      image_option_layout.verification_control.left,
      76,
      image_option_layout.verification_control.width(),
      160,
      TRUE);
  MoveWindow(
      state.source_combo,
      clone_layout.source_control.left,
      236,
      clone_layout.source_control.width(),
      280,
      TRUE);
  MoveWindow(
      state.target_combo,
      clone_layout.target_control.left,
      236,
      clone_layout.target_control.width(),
      280,
      TRUE);
  MoveWindow(
      state.restore_change_image,
      bottom_action_layout.secondary_action.left,
      client.bottom - 72,
      bottom_action_layout.secondary_action.width(),
      42,
      TRUE);
  MoveWindow(
      state.pause_action,
      bottom_action_layout.secondary_action.left,
      client.bottom - 72,
      bottom_action_layout.secondary_action.width(),
      42,
      TRUE);
  const auto diagnostics_buttons = ytec::windowsapp::
      calculate_first_run_guidance_diagnostic_button_layout(
          bottom_action_layout.secondary_action.left,
          bottom_action_layout.secondary_action.right);
  MoveWindow(
      state.manual_update_action,
      diagnostics_buttons.update_left,
      client.bottom - 72,
      diagnostics_buttons.update_width,
      42,
      TRUE);
  MoveWindow(
      state.first_run_guidance_action,
      diagnostics_buttons.guidance_left,
      client.bottom - 72,
      diagnostics_buttons.guidance_width,
      42,
      TRUE);
  MoveWindow(
      state.primary_action,
      bottom_action_layout.primary_action.left,
      client.bottom - 72,
      bottom_action_layout.primary_action.width(),
      42,
      TRUE);
}

void update_navigation_state(AppState& state) {
  for (std::size_t index = 0; index < state.navigation.size(); ++index) {
    InvalidateRect(state.navigation[index], nullptr, TRUE);
  }
}

std::optional<std::size_t> combo_selection(const HWND combo) {
  const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (selection == CB_ERR) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selection);
}

std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) {
    return {};
  }
  std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied = GetWindowTextW(
      control, value.data(), static_cast<int>(value.size()));
  if (copied <= 0) {
    return {};
  }
  value.resize(static_cast<std::size_t>(copied));
  return value;
}

ytec::windowsapp::RescueMediaKind selected_media_kind(
    const AppState& state) {
  return SendMessageW(
             state.media_kind_combo, CB_GETCURSEL, 0, 0) == 1
             ? ytec::windowsapp::RescueMediaKind::usb_drive
             : ytec::windowsapp::RescueMediaKind::iso_file;
}

ytec::imageformat::TransferMode selected_transfer_mode(
    const AppState& state) {
  return SendMessageW(
             state.transfer_mode_combo, CB_GETCURSEL, 0, 0) == 1
             ? ytec::imageformat::TransferMode::shrink
             : ytec::imageformat::TransferMode::exact;
}

std::optional<ytec::imageformat::TsumugiCreateVerificationMode>
selected_image_create_verification_mode(const AppState& state) {
  switch (SendMessageW(
      state.image_verification_mode_combo, CB_GETCURSEL, 0, 0)) {
    case 0:
      return ytec::imageformat::TsumugiCreateVerificationMode::complete;
    case 1:
      return ytec::imageformat::TsumugiCreateVerificationMode::fast;
    default:
      return std::nullopt;
  }
}

std::wstring_view image_create_verification_mode_label(
    const ytec::imageformat::TsumugiCreateVerificationMode mode) noexcept {
  switch (mode) {
    case ytec::imageformat::TsumugiCreateVerificationMode::complete:
      return L"完全（推奨）";
    case ytec::imageformat::TsumugiCreateVerificationMode::fast:
      return L"高速（完成前の追加全走査のみ省略）";
    default:
      return L"未対応";
  }
}

bool selected_clone_rescue_mode(const AppState& state) {
  return state.page == Page::clone &&
         SendMessageW(
             state.transfer_mode_combo, CB_GETCURSEL, 0, 0) == 2;
}

bool selected_image_rescue_mode(const AppState& state) {
  return state.page == Page::create_image &&
         SendMessageW(
             state.transfer_mode_combo, CB_GETCURSEL, 0, 0) == 2;
}

ytec::windowsapp::RescueMediaBootProfile selected_media_profile(
    const AppState& state) {
  return SendMessageW(
             state.media_profile_combo, CB_GETCURSEL, 0, 0) == 1
             ? ytec::windowsapp::RescueMediaBootProfile::
                   windows_uefi_2023_ca
             : ytec::windowsapp::RescueMediaBootProfile::
                   windows_uefi_2011_ca;
}

ytec::windowsapp::RescueUsbProvisioningMode selected_media_usb_mode(
    const AppState& state) {
  return SendMessageW(
             state.media_usb_mode_combo, CB_GETCURSEL, 0, 0) == 1
      ? ytec::windowsapp::RescueUsbProvisioningMode::preserve_data_refresh
      : ytec::windowsapp::RescueUsbProvisioningMode::initialize_all;
}

ytec::windowsapp::RescueUsbDataFileSystem
selected_media_usb_file_system(const AppState& state) {
  return SendMessageW(
             state.media_usb_file_system_combo, CB_GETCURSEL, 0, 0) == 1
      ? ytec::windowsapp::RescueUsbDataFileSystem::exfat
      : ytec::windowsapp::RescueUsbDataFileSystem::ntfs;
}

const ytec::diskmodel::DiskInfo* selected_media_usb_target(
    const AppState& state) {
  const auto index = combo_selection(state.target_combo);
  if (!state.inventory.has_value() || !index.has_value() ||
      index.value() >= state.inventory->disks.size()) {
    return nullptr;
  }
  return &state.inventory->disks[index.value()];
}

ytec::windowsapp::RescueMediaUsbUiView current_rescue_media_usb_ui_view(
    const AppState& state) {
  const auto* evidence = state.media_usb_inspection.has_value() &&
          state.media_usb_inspection->evidence.has_value()
      ? &state.media_usb_inspection->evidence.value()
      : nullptr;
  const auto inspection_state = state.media_usb_inspection_running.load()
      ? ytec::windowsapp::RescueUsbInspectionState::scanning
      : state.media_usb_inspection.has_value()
          ? state.media_usb_inspection->state
          : ytec::windowsapp::RescueUsbInspectionState::not_started;
  return ytec::windowsapp::build_rescue_media_usb_ui_view({
      .usb_kind_selected = selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::usb_drive,
      .selected_target = selected_media_usb_target(state),
      .inspection_state = inspection_state,
      .owned_evidence = evidence,
      .inspection_message = state.media_usb_inspection.has_value()
          ? std::wstring_view(state.media_usb_inspection->message)
          : std::wstring_view{},
      .selected_mode = selected_media_usb_mode(state),
      .selected_file_system = selected_media_usb_file_system(state),
      .operation_running = state.media_creation_running.load(),
  });
}

ytec::windowsapp::RescueMediaPlanView current_rescue_media_plan(
    const AppState& state) {
  const auto usb_view = current_rescue_media_usb_ui_view(state);
  const auto* owned_media = state.media_usb_inspection.has_value() &&
          state.media_usb_inspection->evidence.has_value()
      ? &state.media_usb_inspection->evidence->owned_media
      : nullptr;
  const auto* reviewed_storage_plan = usb_view.storage_plan.has_value()
      ? &usb_view.storage_plan.value()
      : nullptr;
  auto plan = ytec::windowsapp::evaluate_rescue_media_plan({
      .preflight = state.media_preflight.has_value()
                       ? &state.media_preflight.value()
                       : nullptr,
      .kind = selected_media_kind(state),
      .boot_profile = selected_media_profile(state),
      .iso_destination = control_text(state.media_output_edit),
      .inventory = state.inventory.has_value()
                       ? &state.inventory.value()
                       : nullptr,
      .usb_target_index = combo_selection(state.target_combo),
      .usb_provisioning_mode = selected_media_usb_mode(state),
      .usb_data_file_system = selected_media_usb_file_system(state),
      .usb_owned_media = owned_media,
      .reviewed_usb_storage_plan = reviewed_storage_plan,
      .inventory_loading = state.inventory_loading.load(),
  });
  if (selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::usb_drive &&
      !usb_view.ready_for_review && !usb_view.status.empty() &&
      plan.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::usb_inspection_required) {
    plan.message = usb_view.status;
  }
  return plan;
}

ytec::windowsapp::CloneSelectionView current_clone_selection(
    const AppState& state) {
  return ytec::windowsapp::evaluate_clone_selection(
      state.inventory.has_value() ? &state.inventory.value() : nullptr,
      combo_selection(state.source_combo),
      combo_selection(state.target_combo),
      state.inventory_loading.load(),
      selected_transfer_mode(state) ==
          ytec::imageformat::TransferMode::exact);
}

bool current_windows_data_rescue_selection_ready(const AppState& state) {
  if (!selected_clone_rescue_mode(state) ||
      state.inventory_loading.load() || !state.inventory.has_value()) {
    return false;
  }
  const auto source_index = combo_selection(state.source_combo);
  const auto target_index = combo_selection(state.target_combo);
  if (!source_index.has_value() || !target_index.has_value() ||
      source_index.value() == target_index.value() ||
      source_index.value() >= state.inventory->disks.size() ||
      target_index.value() >= state.inventory->disks.size()) {
    return false;
  }
  const auto& source = state.inventory->disks[source_index.value()];
  const auto& target = state.inventory->disks[target_index.value()];
  const bool source_protected =
      source.read_only.value_or(false) || source.offline.value_or(false);
  const bool target_health_allowed =
      ytec::diskmodel::disk_health_operation_advice(
          target.health, false) !=
      ytec::diskmodel::DiskHealthOperationAdvice::block_target;
  return !source.is_system_disk && !target.is_system_disk &&
         source.read_only.has_value() && source.offline.has_value() &&
         source.removable.has_value() && target.offline.has_value() &&
         target.read_only.has_value() && target.removable.has_value() &&
         source_protected && !source.removable.value() &&
         !target.removable.value() && !source.bus_type.empty() &&
         !target.bus_type.empty() && source.size_bytes != 0U &&
         target.size_bytes >= source.size_bytes &&
         source.logical_sector_size == 512U &&
         target.logical_sector_size == 512U && target_health_allowed;
}

std::wstring current_windows_data_rescue_selection_message(
    const AppState& state) {
  if (state.inventory_loading.load()) {
    return L"救出対象を読み取り専用で再確認しています。";
  }
  if (!state.inventory.has_value()) {
    return L"救出対象のディスク一覧を取得できません。";
  }
  const auto source_index = combo_selection(state.source_combo);
  const auto target_index = combo_selection(state.target_combo);
  if (!source_index.has_value() || !target_index.has_value() ||
      source_index.value() >= state.inventory->disks.size() ||
      target_index.value() >= state.inventory->disks.size() ||
      source_index.value() == target_index.value()) {
    return L"別々のコピー元とコピー先を選択してください。";
  }
  const auto& source = state.inventory->disks[source_index.value()];
  const auto& target = state.inventory->disks[target_index.value()];
  if (source.is_system_disk) {
    return L"稼働中Windowsのシステムディスク救出はPE版を使用してください。";
  }
  if (!source.read_only.has_value() || !source.offline.has_value() ||
      (!source.read_only.value() && !source.offline.value())) {
    return L"Windows版はコピー元属性を変更しません。既にread-onlyまたはofflineのデータディスクだけを救出できます。";
  }
  if (source.logical_sector_size != 512U ||
      target.logical_sector_size != 512U) {
    return L"現在のWindows救出モードは512バイト論理セクターだけに対応し、4KnはPEでも安全側に停止します。";
  }
  if (target.size_bytes < source.size_bytes) {
    return L"救出モードは縮小しません。コピー元と同容量以上のコピー先が必要です。";
  }
  if (!current_windows_data_rescue_selection_ready(state)) {
    return L"接続方式、removable属性、コピー先の健康状態など、救出の安全条件を満たしていません。";
  }
  return L"救出モード: 読取不能範囲を再試行し、残る欠損はゼロ埋めmapへ記録します。結果は常に「一部欠損の可能性あり」です。";
}

bool current_windows_data_rescue_image_selection_ready(
    const AppState& state) {
  if (!selected_image_rescue_mode(state) ||
      state.inventory_loading.load() || !state.inventory.has_value()) {
    return false;
  }
  const auto source_index = combo_selection(state.source_combo);
  if (!source_index.has_value() ||
      source_index.value() >= state.inventory->disks.size()) {
    return false;
  }
  const auto& source = state.inventory->disks[source_index.value()];
  const bool source_protected =
      source.read_only.value_or(false) || source.offline.value_or(false);
  return !source.is_system_disk && source.read_only.has_value() &&
         source.offline.has_value() && source.removable.has_value() &&
         source_protected && !source.removable.value() &&
         !source.bus_type.empty() && source.size_bytes != 0U &&
         source.logical_sector_size == 512U &&
         ytec::imageformat::is_supported_sector_size_pair(
             source.logical_sector_size, source.physical_sector_size) &&
         (source.partition_style == ytec::diskmodel::PartitionStyle::gpt ||
          source.partition_style == ytec::diskmodel::PartitionStyle::mbr);
}

std::wstring current_windows_data_rescue_image_selection_message(
    const AppState& state) {
  if (state.inventory_loading.load()) {
    return L"救出イメージ元を読み取り専用で再確認しています。";
  }
  if (!state.inventory.has_value()) {
    return L"救出イメージ元のディスク一覧を取得できません。";
  }
  const auto source_index = combo_selection(state.source_combo);
  if (!source_index.has_value() ||
      source_index.value() >= state.inventory->disks.size()) {
    return L"救出するデータディスクを選択してください。";
  }
  const auto& source = state.inventory->disks[source_index.value()];
  if (source.is_system_disk) {
    return L"稼働中Windowsのシステムディスク救出はPE版を使用してください。";
  }
  if (!source.read_only.has_value() || !source.offline.has_value() ||
      (!source.read_only.value() && !source.offline.value())) {
    return L"Windows版はSource属性を変更しません。既にread-onlyまたはofflineのデータディスクだけを救出できます。";
  }
  if (source.logical_sector_size != 512U) {
    return L"Windows救出イメージは実媒体検証が済むまで512バイト論理セクターだけに限定します。";
  }
  if (!current_windows_data_rescue_image_selection_ready(state)) {
    return L"GPT/MBR、物理sector、接続方式、removable属性など、救出イメージの安全条件を満たしていません。";
  }
  return L"救出イメージ: Sourceを一度だけ所有一時領域へ救出し、封印・完全検証・破棄後に.tsumugi完成名を確定します。";
}

void update_action_state(AppState& state);

bool restore_manifest_partition_selected(
    const ytec::imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              ytec::imageformat::TsumugiManifestPartitionFlags::selected)) !=
      0U;
}

std::optional<std::uint32_t> selected_restore_source_partition(
    const AppState& state) {
  const auto selected = combo_selection(
      state.restore_source_partition_combo);
  if (!selected.has_value() || selected.value() == 0U) {
    return std::nullopt;
  }
  const auto candidate_index = selected.value() - 1U;
  if (candidate_index >= state.restore_source_partition_candidates.size()) {
    return std::nullopt;
  }
  return state.restore_source_partition_candidates[candidate_index];
}

std::optional<ytec::imageformat::
    TsumugiPhysicalIndividualPartitionRestoreSelection>
first_restore_partition_selection(
    const ytec::windowsapp::TsumugiRestoreImagePreflightReport& image,
    const ytec::diskmodel::DiskInfo& target,
    const std::uint32_t source_table_index) {
  auto partitions = target.partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const ytec::diskmodel::PartitionInfo& left,
         const ytec::diskmodel::PartitionInfo& right) {
        if (left.number != right.number) {
          return left.number < right.number;
        }
        if (left.offset_bytes != right.offset_bytes) {
          return left.offset_bytes < right.offset_bytes;
        }
        return left.size_bytes < right.size_bytes;
      });
  for (std::size_t index = 0U; index < partitions.size(); ++index) {
    const auto& partition = partitions[index];
    ytec::imageformat::
        TsumugiPhysicalIndividualPartitionRestoreSelection selection{
            .source_table_index = source_table_index,
            .target = ytec::imageformat::
                TsumugiPhysicalExistingPartitionRestoreSelection{
                    .target_table_index =
                        static_cast<std::uint32_t>(index + 1U),
                    .target_partition_number = partition.number,
                    .target_offset = partition.offset_bytes,
                    .target_size = partition.size_bytes,
                },
        };
    if (ytec::imageformat::
            validate_tsumugi_physical_individual_partition_selection_v1(
                image.manifest, target, selection)) {
      return selection;
    }
  }
  const auto unallocated = ytec::imageformat::
      find_tsumugi_physical_unallocated_restore_candidates_v1(
          image.manifest, target, source_table_index);
  if (unallocated && !unallocated.value().empty()) {
    return unallocated.value().front();
  }
  return std::nullopt;
}

void populate_restore_target_partition_candidates(AppState& state) {
  SendMessageW(
      state.restore_target_partition_combo, CB_RESETCONTENT, 0, 0);
  state.restore_target_partition_candidates.clear();
  const auto source = selected_restore_source_partition(state);
  const auto target_index = combo_selection(state.target_combo);
  if (!source.has_value() || !state.restore_preflight.has_value() ||
      !state.inventory.has_value() || !target_index.has_value() ||
      target_index.value() >= state.inventory->disks.size()) {
    update_action_state(state);
    return;
  }

  const auto& image = state.restore_preflight.value();
  const auto& target = state.inventory->disks[target_index.value()];
  auto partitions = target.partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const ytec::diskmodel::PartitionInfo& left,
         const ytec::diskmodel::PartitionInfo& right) {
        if (left.number != right.number) {
          return left.number < right.number;
        }
        if (left.offset_bytes != right.offset_bytes) {
          return left.offset_bytes < right.offset_bytes;
        }
        return left.size_bytes < right.size_bytes;
      });
  for (std::size_t index = 0U; index < partitions.size(); ++index) {
    const auto& partition = partitions[index];
    ytec::imageformat::
        TsumugiPhysicalIndividualPartitionRestoreSelection selection{
            .source_table_index = source.value(),
            .target = ytec::imageformat::
                TsumugiPhysicalExistingPartitionRestoreSelection{
                    .target_table_index =
                        static_cast<std::uint32_t>(index + 1U),
                    .target_partition_number = partition.number,
                    .target_offset = partition.offset_bytes,
                    .target_size = partition.size_bytes,
                },
        };
    if (!ytec::imageformat::
             validate_tsumugi_physical_individual_partition_selection_v1(
                 image.manifest, target, selection)) {
      continue;
    }
    const std::wstring label =
        L"区画 " + std::to_wstring(partition.number) + L" / " +
        format_bytes(partition.size_bytes) + L" / " +
        (partition.name.empty() ? partition.type : partition.name) +
        L"（既存内容を上書き）";
    SendMessageW(
        state.restore_target_partition_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    state.restore_target_partition_candidates.push_back(
        std::move(selection));
  }
  const auto unallocated = ytec::imageformat::
      find_tsumugi_physical_unallocated_restore_candidates_v1(
          image.manifest, target, source.value());
  if (unallocated) {
    for (const auto& selection : unallocated.value()) {
      const auto& placement = std::get<ytec::imageformat::
          TsumugiPhysicalUnallocatedRestoreSelection>(selection.target);
      const std::wstring label =
          L"未割当 / offset " + format_bytes(placement.target_offset) +
          L" / 新規区画 " + format_bytes(placement.target_size) +
          L"（既存区画は保持）";
      SendMessageW(
          state.restore_target_partition_combo,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(label.c_str()));
      state.restore_target_partition_candidates.push_back(selection);
    }
  }
  if (!state.restore_target_partition_candidates.empty()) {
    SendMessageW(
        state.restore_target_partition_combo, CB_SETCURSEL, 0, 0);
  }
  update_action_state(state);
}

std::optional<ytec::imageformat::
    TsumugiPhysicalIndividualPartitionRestoreSelection>
current_restore_individual_partition_selection(const AppState& state) {
  if (!selected_restore_source_partition(state).has_value()) {
    return std::nullopt;
  }
  const auto target = combo_selection(
      state.restore_target_partition_combo);
  if (!target.has_value() ||
      target.value() >= state.restore_target_partition_candidates.size()) {
    return std::nullopt;
  }
  return state.restore_target_partition_candidates[target.value()];
}

ytec::windowsapp::RestoreTargetSelectionView
current_restore_target_selection(const AppState& state) {
  const auto source = selected_restore_source_partition(state);
  const auto individual =
      current_restore_individual_partition_selection(state);
  if (source.has_value() && !individual.has_value()) {
    return ytec::windowsapp::RestoreTargetSelectionView{
        .issue = ytec::windowsapp::RestoreTargetSelectionIssue::
            individual_selection_invalid,
        .message =
            L"必要容量を満たす既存パーティションまたは未割当候補を選択してください。"};
  }
  return ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
      state.restore_preflight.has_value()
          ? &state.restore_preflight.value()
          : nullptr,
      state.inventory.has_value() ? &state.inventory.value() : nullptr,
      combo_selection(state.target_combo),
      state.inventory_loading.load(),
      individual);
}

void populate_restore_source_partition_candidates(AppState& state) {
  SendMessageW(
      state.restore_source_partition_combo, CB_RESETCONTENT, 0, 0);
  state.restore_source_partition_candidates.clear();
  if (!state.restore_preflight.has_value()) {
    SendMessageW(
        state.restore_target_partition_combo, CB_RESETCONTENT, 0, 0);
    state.restore_target_partition_candidates.clear();
    return;
  }
  SendMessageW(
      state.restore_source_partition_combo,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"ディスク全体を復元"));
  const auto& image = state.restore_preflight.value();
  if (image.manifest.mode !=
      ytec::imageformat::TsumugiManifestMode::shrink) {
    for (const auto& partition : image.manifest.partitions) {
      if (!restore_manifest_partition_selected(partition)) {
        continue;
      }
      const std::wstring label =
          L"画像内の区画 " +
          std::to_wstring(partition.source_partition_number) + L" / " +
          format_bytes(partition.source_size) + L" を個別復元";
      SendMessageW(
          state.restore_source_partition_combo,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(label.c_str()));
      state.restore_source_partition_candidates.push_back(
          partition.source_table_index);
    }
  }
  const bool partition_selection =
      (static_cast<std::uint32_t>(image.manifest.flags) &
       static_cast<std::uint32_t>(
           ytec::imageformat::TsumugiManifestFlags::partition_selection)) !=
      0U;
  const WPARAM default_index =
      partition_selection &&
              !state.restore_source_partition_candidates.empty()
          ? 1U
          : 0U;
  SendMessageW(
      state.restore_source_partition_combo,
      CB_SETCURSEL,
      default_index,
      0);
  populate_restore_target_partition_candidates(state);
}

void select_default_restore_target(AppState& state) {
  SendMessageW(
      state.target_combo,
      CB_SETCURSEL,
      static_cast<WPARAM>(-1),
      0);
  if (!state.restore_preflight.has_value() ||
      !state.inventory.has_value() ||
      state.inventory_loading.load()) {
    return;
  }

  for (std::size_t index = 0;
       index < state.inventory->disks.size();
       ++index) {
    const auto source = selected_restore_source_partition(state);
    const auto individual = source.has_value()
        ? first_restore_partition_selection(
              state.restore_preflight.value(),
              state.inventory->disks[index],
              source.value())
        : std::nullopt;
    if (source.has_value() && !individual.has_value()) {
      continue;
    }
    const auto candidate =
        ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
            &state.restore_preflight.value(),
            &state.inventory.value(),
            index,
            false,
            individual);
    if (candidate.ready_for_confirmation) {
      SendMessageW(
          state.target_combo,
          CB_SETCURSEL,
          static_cast<WPARAM>(index),
          0);
      populate_restore_target_partition_candidates(state);
      return;
    }
  }
}

void update_action_state(AppState& state);

void start_rescue_usb_inspection(AppState& state) {
  if (state.media_usb_inspection_running.load()) {
    return;
  }
  if (state.media_usb_inspection_thread.joinable()) {
    state.media_usb_inspection_thread.join();
  }
  state.media_usb_inspection.reset();
  const auto* target = selected_media_usb_target(state);
  if (target == nullptr) {
    update_action_state(state);
    InvalidateRect(state.window, nullptr, TRUE);
    return;
  }
  const auto safe_target = ytec::windowsapp::plan_rescue_usb_storage({
      .target = target,
      .mode = ytec::windowsapp::
          RescueUsbProvisioningMode::initialize_all,
      .data_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
  });
  if (!safe_target) {
    state.media_usb_inspection =
        ytec::windowsapp::RescueUsbInspectionResult{
            .state = ytec::windowsapp::
                RescueUsbInspectionState::blocked,
            .message = safe_target.error().message,
            .physical_write_started = false,
        };
    update_action_state(state);
    InvalidateRect(state.window, nullptr, TRUE);
    return;
  }

  const auto expected_target = safe_target.value().expected_target;
  const auto expected_layout = safe_target.value().reviewed_layout;
  const auto target_copy = *target;
  state.media_usb_inspection_cancel_requested.store(false);
  state.media_usb_inspection_running.store(true);
  EnableWindow(state.refresh, FALSE);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND window = state.window;
  auto* const cancellation =
      &state.media_usb_inspection_cancel_requested;
  state.media_usb_inspection_thread = std::thread(
      [window,
       cancellation,
       expected_target,
       expected_layout,
       target_copy]() mutable {
        auto payload = std::make_unique<RescueUsbInspectionPayload>();
        payload->expected_target = std::move(expected_target);
        payload->expected_layout = std::move(expected_layout);
        payload->result = ytec::windowsapp::
            inspect_rescue_usb_owned_media_with_windows_apis(
                target_copy, cancellation);
        if (PostMessageW(
                window,
                kRescueUsbInspectionCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void select_default_media_usb_target(AppState& state) {
  if (state.media_usb_inspection_running.load()) {
    return;
  }
  state.media_usb_inspection.reset();
  SendMessageW(
      state.target_combo,
      CB_SETCURSEL,
      static_cast<WPARAM>(-1),
      0);
  if (!state.media_preflight.has_value() ||
      !state.inventory.has_value() ||
      state.inventory_loading.load()) {
    return;
  }

  for (std::size_t index = 0;
       index < state.inventory->disks.size();
       ++index) {
    const auto candidate = ytec::windowsapp::plan_rescue_usb_storage({
        .target = &state.inventory->disks[index],
        .mode = ytec::windowsapp::
            RescueUsbProvisioningMode::initialize_all,
        .data_file_system =
            ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
    });
    if (candidate.has_value()) {
      SendMessageW(
          state.target_combo,
          CB_SETCURSEL,
          static_cast<WPARAM>(index),
          0);
      start_rescue_usb_inspection(state);
      return;
    }
  }
  state.media_usb_inspection.reset();
}

void start_manual_update_check(AppState& state) {
  if (state.manual_update_running.exchange(true)) {
    return;
  }
  if (state.manual_update_thread.joinable()) {
    state.manual_update_thread.join();
  }
  state.manual_update_report.reset();
  state.manual_update_error.clear();
  if (state.logger.has_value()) {
    state.logger->info(L"利用者操作による手動更新確認を開始");
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  state.manual_update_thread = std::thread([window]() {
    auto payload = std::make_unique<ManualUpdatePayload>();
#if defined(YTEC_UI_ACCEPTANCE_BUILD)
    const auto fake_transport = [](
        const std::wstring_view fixed_url,
        const std::size_t maximum_response_bytes) {
      constexpr std::string_view kBody{
          "{\"schemaVersion\":1,"
          "\"productId\":\"ytec-tsumugi-drive\","
          "\"latestVersion\":\"" YTEC_PRODUCT_VERSION "\","
          "\"releasePageUrl\":\"https://ytec.cloudfree.jp/forge/projects/"
          "tsumugi-drive/\","
          "\"publishedUtc\":\"2026-08-11T00:00:00Z\","
          "\"packageSha256\":\""
          "0000000000000000000000000000000000000000000000000000000000000000"
          "\"}"};
      std::vector<std::byte> body;
      if (fixed_url == ytec::windowsapp::kManualUpdateManifestUrl &&
          kBody.size() <= maximum_response_bytes) {
        body.reserve(kBody.size());
        for (const char value : kBody) {
          body.push_back(static_cast<std::byte>(
              static_cast<unsigned char>(value)));
        }
      }
      return ytec::clonecore::Result<
          ytec::windowsapp::ManualUpdateTransportResponse>::success(
          ytec::windowsapp::ManualUpdateTransportResponse{
              .http_status = 200U,
              .final_url = std::wstring(fixed_url),
              .content_type = L"application/json",
              .body = std::move(body),
          });
    };
    auto result = ytec::windowsapp::check_manual_update(
        ytec::windowsapp::ManualUpdateCheckRequest{
            .user_initiated = true,
            .current_version = std::string(kAppVersion),
        },
        fake_transport);
#else
    auto result = ytec::windowsapp::check_manual_update_with_windows_apis(
        ytec::windowsapp::ManualUpdateCheckRequest{
            .user_initiated = true,
            .current_version = std::string(kAppVersion),
        });
#endif
    if (result.has_value()) {
      payload->report = result.take_value();
    } else {
      payload->error = result.error();
    }
    if (PostMessageW(
            window,
            kManualUpdateCompleteMessage,
            0,
            reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
      static_cast<void>(payload.release());
    }
  });
}

void choose_support_zip_destination(AppState& state) {
  if (state.support_zip_planning.load() ||
      state.support_zip_creation_running.load() ||
      state.clone_running.load() ||
      state.backup_running.load() ||
      state.restore_running.load() ||
      state.media_creation_running.load() ||
      state.inventory_loading.load() ||
      state.manual_update_running.load() ||
      state.adk_management_running.load() ||
      state.media_preflight_running.load() ||
      state.media_usb_inspection_running.load() ||
      state.restore_preflight_running.load()) {
    return;
  }
  std::vector<wchar_t> path(32768, L'\0');
  const SYSTEMTIME now = [] {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    return value;
  }();
  swprintf_s(
      path.data(),
      path.size(),
      L"Tsumugi-support-%04u%02u%02u-%02u%02u%02u.zip",
      now.wYear,
      now.wMonth,
      now.wDay,
      now.wHour,
      now.wMinute,
      now.wSecond);
  constexpr wchar_t kFilter[] =
      L"ZIPアーカイブ (*.zip)\0*.zip\0すべてのファイル (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"zip";
  dialog.lpstrTitle = L"サポートZIPのローカル保存先";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
      OFN_DONTADDTORECENT | OFN_NOREADONLYRETURN | OFN_OVERWRITEPROMPT;
#if defined(YTEC_UI_ACCEPTANCE_BUILD)
  const std::array<ytec::windowsapp::SupportZipDisplayEntry, 3>
      kSyntheticEntries{{
          {.archive_entry_name = L"tsumugi-20260813-101500.log",
           .source_size_bytes = 32768,
           .masked_size_bytes = 24576},
          {.archive_entry_name = L"tsumugi-failure-20260812-204500.log",
           .source_size_bytes = 16384,
           .masked_size_bytes = 12288},
          {.archive_entry_name = L"tsumugi-20260811-093000.log",
           .source_size_bytes = 8192,
           .masked_size_bytes = 6144},
      }};
  const auto review = ytec::windowsapp::build_support_zip_review_model(
      L"C:\\Synthetic\\Tsumugi-support-20260813.zip",
      kSyntheticEntries,
      43008,
      2);
  state.support_zip_status = review_support_zip_model(
      state.window, state.body_font, review)
      ? L"UI受入: 明示作成を選択（ファイルI/Oは実行していません）"
      : L"UI受入: Esc／キャンセル（ファイルI/Oは実行していません）";
  InvalidateRect(state.window, nullptr, TRUE);
  return;
#else
  if (GetSaveFileNameW(&dialog) == FALSE) {
    return;
  }
  const DWORD existing_attributes = GetFileAttributesW(path.data());
  if (existing_attributes != INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(
        state.window,
        L"既存ファイルは上書きしません。別の新しいZIP名を選んでください。",
        L"新しい保存名が必要です",
        MB_OK | MB_ICONINFORMATION);
    return;
  }
  state.support_zip_report.reset();
  state.support_zip_error.clear();
  state.support_zip_status = L"製品ログを追加マスクし、含有一覧を準備しています…";
  state.support_zip_planning.store(true);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  if (state.support_zip_thread.joinable()) {
    state.support_zip_thread.join();
  }
  const HWND window = state.window;
  const std::wstring final_path(path.data());
  state.support_zip_thread = std::thread([window, final_path]() {
    auto payload = std::make_unique<SupportZipPlanPayload>();
    auto result = ytec::windowsapp::plan_current_executable_support_zip(
        final_path);
    if (result) {
      payload->plan = result.take_value();
    } else {
      payload->error = result.error();
    }
    if (PostMessageW(
            window,
            kSupportZipPlanCompleteMessage,
            0,
            reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
      static_cast<void>(payload.release());
    }
  });
#endif
}

void start_support_zip_creation(
    AppState& state,
    ytec::windowsapp::SupportZipPlan plan) {
  state.support_zip_status = L"サポートZIPをローカル作成・完全検証しています…";
  state.support_zip_creation_running.store(true);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  if (state.support_zip_thread.joinable()) {
    state.support_zip_thread.join();
  }
  const HWND window = state.window;
  state.support_zip_thread = std::thread(
      [window, plan = std::move(plan)]() mutable {
        auto payload = std::make_unique<SupportZipCreationPayload>();
        auto result = ytec::windowsapp::create_windows_support_zip(plan);
        if (result) {
          payload->report = result.take_value();
        } else {
          payload->error = result.error();
        }
        if (PostMessageW(
                window,
                kSupportZipCreationCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void open_manual_update_release_page(AppState& state) {
  const std::wstring url(ytec::windowsapp::kManualUpdateReleasePageUrl);
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      state.window,
      L"open",
      url.c_str(),
      nullptr,
      nullptr,
      SW_SHOWNORMAL));
  if (result <= 32) {
    MessageBoxW(
        state.window,
        (L"Y-TEC公式配布ページを既定のブラウザーで開けませんでした。\n\n" +
         url)
            .c_str(),
        L"配布ページを開けませんでした",
        MB_OK | MB_ICONERROR);
  }
}

void start_media_preflight(AppState& state) {
  if (state.media_preflight_running.exchange(true)) {
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"レスキューメディア作成環境の読み取り専用診断開始");
  }
  state.media_preflight.reset();
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  state.media_preflight_thread = std::thread([window]() {
    auto payload = std::make_unique<MediaPreflightPayload>();
    payload->view =
        ytec::windowsapp::inspect_local_windows_media_environment();
    if (PostMessageW(
            window,
            kMediaPreflightCompleteMessage,
            0,
            reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
      static_cast<void>(payload.release());
    }
  });
}

void open_official_adk_page(
    AppState& state,
    const wchar_t* const url,
    const std::wstring_view page_name) {
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      state.window,
      L"open",
      url,
      nullptr,
      nullptr,
      SW_SHOWNORMAL));
  if (result <= 32) {
    MessageBoxW(
        state.window,
        (std::wstring(page_name) +
         L"を既定のブラウザーで開けませんでした。\n\n" + url)
            .c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return;
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"Microsoft公式ADK案内を既定ブラウザーで表示 page=" +
        std::wstring(page_name));
  }
}

HRESULT CALLBACK adk_management_dialog_callback(
    HWND,
    const UINT notification,
    WPARAM,
    const LPARAM parameter,
    const LONG_PTR callback_data) {
  if (notification == TDN_HYPERLINK_CLICKED && callback_data != 0 &&
      parameter != 0) {
    auto* const state = reinterpret_cast<AppState*>(callback_data);
    const auto* const clicked = reinterpret_cast<const wchar_t*>(parameter);
    if (std::wstring_view(clicked) == kOfficialAdkInstallGuideUrl) {
      open_official_adk_page(
          *state,
          kOfficialAdkInstallGuideUrl,
          L"ADK／WinPE導入ページ");
    }
  }
  return S_OK;
}

void start_adk_management_action(
    AppState& state,
    const ytec::windowsapp::AdkManagementAction action) {
  if (state.adk_management_running.load() ||
      state.clone_running.load() || state.backup_running.load() ||
      state.restore_running.load() ||
      state.media_creation_running.load() ||
      state.media_preflight_running.load() ||
      state.support_zip_planning.load() ||
      state.support_zip_creation_running.load()) {
    return;
  }
  if (state.adk_management_thread.joinable()) {
    state.adk_management_thread.join();
  }

  const auto manifest =
      ytec::windowsapp::tsumugi_1_0_0_adk_manifest();
  const auto view =
      ytec::windowsapp::build_adk_management_view(manifest);
  ytec::windowsapp::AdkManagementRequest request{
      .action = action,
      .administrator = state.elevated,
      .explicit_start_confirmed = true,
  };
  // The current release gate is closed. In particular, do not display a
  // path picker or acquire an EULA document before that pure gate is open.
  // execute_adk_management_action repeats the gate as its first operation;
  // the factory lambdas below therefore remain uninvoked in this release.
  if (!view.path_selection_permitted) {
    request.offline_layout_root.clear();
  }

  state.adk_management_status =
      view.execution_gate_open
          ? L"明示されたADK管理操作をバックグラウンドで確認しています…"
          : L"ADK安全ゲートを確認中です。通信・UAC・installer・フォルダー参照は開始しません。";
  state.adk_management_running.store(true);
  if (state.logger.has_value()) {
    state.logger->info(
        L"利用者操作によるADK取得・管理の安全ゲート確認開始");
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  state.adk_management_thread = std::thread(
      [window, manifest, request = std::move(request)]() mutable {
        auto payload = std::make_unique<AdkManagementPayload>();
        auto result = ytec::windowsapp::execute_adk_management_action(
            manifest,
            request,
            ytec::windowsapp::AdkManagementDependencies{
                .make_acquisition_platform = []() {
                  return ytec::windowsapp::
                      make_windows_adk_acquisition_platform();
                },
                .make_management_platform = []() {
                  return ytec::windowsapp::
                      make_windows_adk_management_platform();
                },
            });
        if (result.has_value()) {
          payload->report = result.take_value();
        } else {
          payload->error = result.error();
        }
        if (PostMessageW(
                window,
                kAdkManagementCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void show_adk_management_dialog(AppState& state) {
  const auto manifest =
      ytec::windowsapp::tsumugi_1_0_0_adk_manifest();
  const auto view =
      ytec::windowsapp::build_adk_management_view(manifest);
  constexpr auto contract =
      ytec::windowsapp::adk_management_ui_contract();
  const TASKDIALOG_BUTTON buttons[]{
      {kAdkOfficialDownloadInstallId,
       contract.official_download_label.data()},
      {kAdkOfflineLayoutInstallId,
       contract.offline_install_label.data()},
      {kAdkCreateOfflineLayoutId,
       contract.create_layout_label.data()},
      {kAdkUninstallManagedId,
       contract.uninstall_label.data()},
  };
  const std::wstring pending_label =
      L"（安全ゲート停止中）Enterで停止理由を確認";
  std::array<std::wstring, 4U> gated_labels;
  std::array<TASKDIALOG_BUTTON, 4U> gated_buttons{};
  const TASKDIALOG_BUTTON* displayed_buttons = buttons;
  if (!view.execution_gate_open) {
    for (std::size_t index = 0U; index < std::size(buttons); ++index) {
      gated_labels[index] =
          std::wstring(buttons[index].pszButtonText) + L"\n" +
          pending_label;
      gated_buttons[index] = TASKDIALOG_BUTTON{
          buttons[index].nButtonID,
          gated_labels[index].c_str(),
      };
    }
    displayed_buttons = gated_buttons.data();
  }
  std::wstring content =
      view.status + L"\n\n対象ADK: " + manifest.tested_adk_version +
      L" / 固定取得物: " +
      std::to_wstring(manifest.payloads.size()) + L"件";
  std::wstring payload_details =
      L"固定取得物（この製品には同梱しません）";
  for (const auto& row : view.payload_rows) {
    payload_details += L"\n・" + row;
  }
  payload_details +=
      L"\n\n取得・導入系は、検証済みADK固有EULA全文を同じ画面で提示して明示同意できる場合だけ進みます。";

  const HWND previous_focus = GetFocus();
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = state.window;
  config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS;
  config.dwCommonButtons =
      TDCBF_RETRY_BUTTON | TDCBF_CANCEL_BUTTON;
  config.pszWindowTitle = contract.title.data();
  config.pszMainIcon = view.execution_gate_open
      ? TD_INFORMATION_ICON
      : TD_WARNING_ICON;
  config.pszMainInstruction = view.execution_gate_open
      ? L"Microsoft公式取得物だけを明示操作で管理します"
      : L"現リリースはADK自動取得の安全ゲートを閉じています";
  config.pszContent = content.c_str();
  config.pszExpandedInformation = payload_details.c_str();
  config.pszExpandedControlText = L"固定取得物の要約を表示";
  config.pszCollapsedControlText = L"固定取得物の要約を隠す";
  const std::wstring footer =
      L"<a href=\"" + std::wstring(kOfficialAdkInstallGuideUrl) +
      L"\">Microsoft公式のADK案内</a>（利用者が選んだ場合だけ開きます）";
  config.pszFooter = footer.c_str();
  config.cButtons = static_cast<UINT>(std::size(buttons));
  config.pButtons = displayed_buttons;
  config.nDefaultButton = kAdkOfficialDownloadInstallId;
  config.pfCallback = adk_management_dialog_callback;
  config.lpCallbackData = reinterpret_cast<LONG_PTR>(&state);

  using TaskDialogIndirectFunction = HRESULT(WINAPI*)(
      const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE common_controls = LoadLibraryW(L"comctl32.dll");
  TaskDialogIndirectFunction task_dialog{};
  if (common_controls != nullptr) {
    task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
        GetProcAddress(common_controls, "TaskDialogIndirect"));
    if (task_dialog == nullptr) {
      task_dialog = reinterpret_cast<TaskDialogIndirectFunction>(
          GetProcAddress(
              common_controls,
              MAKEINTRESOURCEA(345)));
    }
  }

  int pressed{};
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result)) {
    MessageBoxW(
        state.window,
        (content +
         L"\n\nADK取得・管理の選択画面を表示できなかったため、何も開始していません。")
            .c_str(),
        contract.title.data(),
        MB_OK | MB_ICONWARNING);
    if (previous_focus != nullptr && IsWindow(previous_focus) != FALSE) {
      SetFocus(previous_focus);
    }
    return;
  }
  if (pressed == kAdkOfficialDownloadInstallId) {
    start_adk_management_action(
        state,
        ytec::windowsapp::AdkManagementAction::
            official_download_install);
  } else if (pressed == kAdkOfflineLayoutInstallId) {
    start_adk_management_action(
        state,
        ytec::windowsapp::AdkManagementAction::offline_layout_install);
  } else if (pressed == kAdkCreateOfflineLayoutId) {
    start_adk_management_action(
        state,
        ytec::windowsapp::AdkManagementAction::create_offline_layout);
  } else if (pressed == kAdkUninstallManagedId) {
    start_adk_management_action(
        state,
        ytec::windowsapp::AdkManagementAction::uninstall_managed);
  } else if (pressed == IDRETRY) {
    start_media_preflight(state);
  }
  if (previous_focus != nullptr && IsWindow(previous_focus) != FALSE) {
    SetFocus(previous_focus);
  }
}

void choose_media_iso_destination(AppState& state) {
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const std::wstring current = control_text(state.media_output_edit);
  const std::wstring initial =
      current.empty() ? L"Tsumugi-Drive-Rescue.iso" : current;
  std::copy(initial.begin(), initial.end(), path.begin());
  constexpr wchar_t kFilter[] =
      L"ISOイメージ (*.iso)\0*.iso\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"iso";
  dialog.lpstrTitle = L"新しく作成するレスキューISOの保存先";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
                 OFN_NOREADONLYRETURN;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      MessageBoxW(
          state.window,
          L"ISO保存先の選択画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  const std::wstring selected(path.data());
  if (!ytec::windowsapp::is_safe_iso_destination_syntax(selected)) {
    MessageBoxW(
        state.window,
        L"ISOはローカルドライブ上の新しい絶対パス（.iso）を指定してください。",
        L"ISO保存先を使用できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  const DWORD attributes = GetFileAttributesW(selected.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(
        state.window,
        L"既存ファイルは上書きしません。別の新しいISO名を指定してください。",
        L"ISO保存先を使用できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  if (GetLastError() != ERROR_FILE_NOT_FOUND) {
    MessageBoxW(
        state.window,
        L"ISO保存先が未使用であることを安全に確認できませんでした。",
        L"ISO保存先を使用できません",
        MB_OK | MB_ICONWARNING);
    return;
  }

  SetWindowTextW(state.media_output_edit, selected.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
}

void start_rescue_media_creation(
    AppState& state,
    const ytec::windowsapp::RescueMediaPlanView& plan) {
  if (state.media_creation_running.load()) {
    return;
  }
  const auto kind = selected_media_kind(state);
  if (!state.elevated) {
    const std::wstring media_name =
        kind == ytec::windowsapp::RescueMediaKind::usb_drive
            ? L"USB"
            : L"ISO";
    const std::wstring administrator_message =
        L"作成内容は確認できました。\n\n"
        L"実際のWIM構成と" + media_name +
        L"作成には管理者権限が必要です。"
        L"\nこの画面からUACを自動表示せず、後ほど管理者として起動して"
        L"同じ作成先を選択してください。";
    const std::wstring administrator_title =
        media_name + L"作成は管理者確認待ちです";
    MessageBoxW(
        state.window,
        administrator_message.c_str(),
        administrator_title.c_str(),
        MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring final_path = control_text(state.media_output_edit);
  std::vector<ytec::clonecore::StableDiskIdentity> protected_log_disks;
  std::optional<ytec::clonecore::Error> output_identity_error;
  if (kind == ytec::windowsapp::RescueMediaKind::usb_drive) {
    if (plan.usb_target_identity.has_value()) {
      protected_log_disks.push_back(plan.usb_target_identity.value());
    }
  } else {
    auto output_identity =
        identify_local_path_backing_read_only(final_path);
    if (output_identity) {
      protected_log_disks.push_back(output_identity.take_value());
    } else {
      output_identity_error = output_identity.error();
    }
  }
  if (!prepare_source_safe_product_logging(
          state,
          protected_log_disks,
          L"レスキューメディア作成") ||
      !require_startup_write_access(
          state, L"レスキューメディア作成")) {
    return;
  }
  if (output_identity_error.has_value()) {
    log_error_summary(
        state.logger,
        L"ISO保存先の物理ディスク識別失敗（RAM隔離で継続）",
        output_identity_error.value());
  }
  if (!confirm_long_operation_power(
          state, L"レスキューメディア作成")) {
    return;
  }

  std::optional<ytec::windowsapp::RescueUsbTargetAuthorization>
      usb_authorization;
  std::optional<ytec::windowsapp::RescueUsbDriveLetterResolution>
      usb_mapping;
  std::wstring confirmation =
      L"手順 3/4  最終確認\n\n" + plan.summary;
  if (kind == ytec::windowsapp::RescueMediaKind::usb_drive) {
    if (!plan.usb_storage_plan.has_value()) {
      MessageBoxW(
          state.window,
          L"検査・レビュー済みのUSB媒体計画がありません。USBを選び直してください。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
      return;
    }
    const bool refresh = plan.usb_storage_plan->mode ==
        ytec::windowsapp::RescueUsbProvisioningMode::preserve_data_refresh;
    confirmation += refresh
        ? L"\n\nローカルADKのMicrosoft公式作成処理で、検証済みY-TEC媒体の"
          L"起動／アプリ領域だけを非上書き更新し、データ領域の全内容を保持します。"
          L"\n更新直前・切替時・完了後に同じデータtreeを再照合します。"
          L"\n\nデータ保持更新を続けますか？"
        : L"\n\nローカルADKのMicrosoft公式作成処理で、選択USB全体を消去し、"
          L"MBR／正確に4GiB FAT32起動領域＋残容量データ領域へ初期化します。"
          L"\n選択していないディスクやパーティションは処理しません。"
          L"\n\nこのUSBの全内容を消去して続けますか？";
  } else {
    confirmation +=
        L"\n\nローカルADKから新しいISOを作成します。"
        L"\n既存ファイルは上書きせず、完成後にISO全体のSHA-256を検証します。"
        L"\n\n作成を開始しますか？";
  }
  if (MessageBoxW(
          state.window,
          confirmation.c_str(),
          kind == ytec::windowsapp::RescueMediaKind::usb_drive
              ? L"レスキューUSBの最終確認 1/2"
              : L"レスキューISOの最終確認",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  if (kind == ytec::windowsapp::RescueMediaKind::usb_drive) {
    if (!plan.usb_target_identity.has_value() ||
        plan.confirmation_token.empty()) {
      MessageBoxW(
          state.window,
          L"USBの安定識別情報または確認語「OK」がありません。"
          L"\n診断情報を更新して、USBを選び直してください。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
      return;
    }
    const auto& expected = plan.usb_target_identity.value();
    const bool refresh = plan.usb_storage_plan.has_value() &&
        plan.usb_storage_plan->mode == ytec::windowsapp::
            RescueUsbProvisioningMode::preserve_data_refresh;
    const std::wstring serial =
        expected.serial_suffix.empty()
            ? L"取得できません"
            : std::wstring(
                  expected.serial_suffix.begin(),
                  expected.serial_suffix.end());
    ConfirmationDialogState dialog_state{
        .details =
            L"作成先: ディスク " +
            std::to_wstring(expected.disk_number) + L" / " +
            expected.model + L"\r\n容量: " +
            format_bytes(expected.size_bytes) +
            L" / シリアル末尾: " + serial +
            (refresh
                 ? L"\r\n保持対象: データ領域の全内容"
                   L"\r\n置換対象: 起動／アプリ領域だけ（非上書き切替）"
                 : L"\r\n削除対象: USBディスク全体（既存パーティションを含む全内容）"
                   L"\r\n作成構成: 4GiB FAT32＋残容量データ領域"),
        .token = plan.confirmation_token,
        .confirm_button_label = refresh
            ? L"データ保持更新を開始"
            : L"USB初期化を開始",
        .font = state.body_font,
    };
    const INT_PTR dialog_result = DialogBoxParamW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
        state.window,
        confirmation_dialog_proc,
        reinterpret_cast<LPARAM>(&dialog_state));
    if (dialog_result != IDOK) {
      if (dialog_result == -1) {
        MessageBoxW(
            state.window,
            L"USB確認画面を開けませんでした。",
            kWindowTitle,
            MB_OK | MB_ICONERROR);
      }
      return;
    }

    auto inventory_provider =
        ytec::diskmodel::make_windows_disk_inventory_provider(
            state.logger.has_value() ? &state.logger.value() : nullptr);
    auto fresh_inventory = inventory_provider->enumerate();
    if (!fresh_inventory) {
      show_product_error(
          state.window,
          L"USBを再確認できませんでした",
          fresh_inventory.error());
      return;
    }
    auto authorization =
        ytec::windowsapp::authorize_rescue_usb_target({
            .expected_target = expected,
            .fresh_inventory = &fresh_inventory.value(),
            .first_step_acknowledged = true,
            .typed_confirmation = plan.confirmation_token,
            .reviewed_storage_plan = plan.usb_storage_plan,
            .fresh_owned_media = refresh &&
                    state.media_usb_inspection.has_value() &&
                    state.media_usb_inspection->evidence.has_value()
                ? &state.media_usb_inspection->evidence->owned_media
                : nullptr,
        });
    if (!authorization) {
      show_product_error(
          state.window,
          L"USBの再確認で停止しました",
          authorization.error());
      return;
    }
    const auto target = std::find_if(
        fresh_inventory.value().disks.begin(),
        fresh_inventory.value().disks.end(),
        [&](const auto& disk) {
          return disk.disk_number ==
                 authorization.value().target.disk_number;
        });
    if (target == fresh_inventory.value().disks.end()) {
      MessageBoxW(
          state.window,
          L"確認済みUSBを再列挙結果から取得できません。",
          L"USBの再確認で停止しました",
          MB_OK | MB_ICONWARNING);
      return;
    }
    auto mapping =
        ytec::windowsapp::
            resolve_windows_rescue_usb_drive_letter_for_plan_read_only(
                *target,
                plan.usb_storage_plan.value(),
                ytec::windowsapp::
                    RescueUsbDestinationVerificationPoint::before_write);
    if (!mapping) {
      show_product_error(
          state.window,
          L"USBのドライブ文字を確定できません",
          mapping.error());
      return;
    }
    usb_authorization = authorization.take_value();
    usb_mapping = mapping.take_value();
  }

  state.media_creation_cancel_requested.store(false);
  state.media_creation_progress.reset();
  state.media_creation_report.reset();
  state.last_logged_media_stage.reset();
  state.media_creation_started_tick = GetTickCount64();
  state.media_creation_running.store(true);
  if (state.logger.has_value()) {
    state.logger->info(
        L"レスキューメディア作成開始 kind=" +
        std::wstring(
            kind == ytec::windowsapp::RescueMediaKind::usb_drive
                ? L"usb"
                : L"iso") +
        L" boot_profile=" +
        std::wstring(
            ytec::windowsapp::rescue_media_boot_profile_label(
                selected_media_profile(state))));
  }
  SetTimer(state.window, kUiRefreshTimerId, 1000U, nullptr);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  auto request = ytec::windowsapp::RescueMediaCreationRequest{
      .kind = kind,
      .boot_profile = selected_media_profile(state),
      .final_iso_path = final_path,
      .administrator = state.elevated,
      .usb_authorization = std::move(usb_authorization),
      .usb_mapping = std::move(usb_mapping),
      .callbacks =
          {
              .progress =
                  [window](const auto& progress) {
                    auto payload =
                        std::make_unique<MediaCreationProgressPayload>();
                    payload->progress = progress;
                    if (PostMessageW(
                            window,
                            kMediaCreationProgressMessage,
                            0,
                            reinterpret_cast<LPARAM>(
                                payload.get())) != FALSE) {
                      static_cast<void>(payload.release());
                    }
                  },
              .cancellation_requested =
                  [&state] {
                    return state.media_creation_cancel_requested.load();
                  },
          },
  };
  const std::uint64_t completion_power_operation_binding =
      ytec::windowsapp::take_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  state.media_creation_thread = std::thread(
      [window,
       cancellation = &state.media_creation_cancel_requested,
       request = std::move(request),
       completion_power_operation_binding]() {
        auto payload = std::make_unique<MediaCreationPayload>();
        payload->requested_kind = request.kind;
        payload->completion_power_operation_binding =
            completion_power_operation_binding;
        ThreadSleepPrevention sleep_prevention;
        if (!sleep_prevention.active()) {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"レスキューメディア作成中の自動スリープ防止",
              .message = L"自動スリープを安全に防止できないため開始しません",
          };
        } else {
          if (request.kind ==
                  ytec::windowsapp::RescueMediaKind::usb_drive &&
              request.usb_authorization.has_value() &&
              request.usb_authorization->storage_plan.has_value() &&
              request.usb_authorization->storage_plan->mode ==
                  ytec::windowsapp::RescueUsbProvisioningMode::
                      preserve_data_refresh) {
            const auto fresh_inspection = ytec::windowsapp::
                reinspect_rescue_usb_storage_plan_with_windows_apis(
                    request.usb_authorization->storage_plan.value(),
                    cancellation);
            if (!fresh_inspection) {
              payload->error = fresh_inspection.error();
            }
          }
          if (!payload->error.has_value()) {
            auto result = ytec::windowsapp::
                execute_rescue_media_creation_with_windows_apis(request);
            if (result) {
              payload->report = result.take_value();
            } else {
              payload->error = result.error();
            }
          }
        }
        payload->sleep_prevention_release = sleep_prevention.release();
        if (PostMessageW(
                window,
                kMediaCreationCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void review_rescue_media_plan(AppState& state) {
  const auto plan = current_rescue_media_plan(state);
  if (!plan.ready_for_confirmation) {
    return;
  }
  const auto kind = selected_media_kind(state);
  std::vector<ytec::clonecore::StableDiskIdentity> protected_log_disks;
  std::optional<ytec::clonecore::Error> output_identity_error;
  if (kind == ytec::windowsapp::RescueMediaKind::usb_drive) {
    if (plan.usb_target_identity.has_value()) {
      protected_log_disks.push_back(plan.usb_target_identity.value());
    }
  } else {
    auto output_identity = identify_local_path_backing_read_only(
        control_text(state.media_output_edit));
    if (output_identity) {
      protected_log_disks.push_back(output_identity.take_value());
    } else {
      output_identity_error = output_identity.error();
    }
  }
  if (!prepare_source_safe_product_logging(
          state,
          protected_log_disks,
          L"レスキューメディア作成") ||
      !require_startup_write_access(
          state, L"レスキューメディア作成")) {
    return;
  }
  if (output_identity_error.has_value()) {
    log_error_summary(
        state.logger,
        L"ISO保存先の物理ディスク識別失敗（RAM隔離で継続）",
        output_identity_error.value());
  }

  std::wstring message =
      L"手順 3/4  作成内容の確認\n\n" + plan.summary;
  if (kind ==
      ytec::windowsapp::RescueMediaKind::usb_drive) {
    const auto target_index = combo_selection(state.target_combo);
    if (!state.inventory.has_value() ||
        !target_index.has_value() ||
        target_index.value() >= state.inventory->disks.size()) {
      return;
    }
    if (!plan.usb_storage_plan.has_value()) {
      MessageBoxW(
          state.window,
          L"検査・レビュー済みのUSB媒体計画がありません。USBを選び直してください。",
          L"USBの安全照合で停止しました",
          MB_OK | MB_ICONWARNING);
      return;
    }
    const auto mapping =
        ytec::windowsapp::
            resolve_windows_rescue_usb_drive_letter_for_plan_read_only(
                state.inventory->disks[target_index.value()],
                plan.usb_storage_plan.value(),
                ytec::windowsapp::
                    RescueUsbDestinationVerificationPoint::before_write);
    if (!mapping) {
      log_error_summary(
          state.logger,
          L"USB物理ディスクとドライブ文字の照合失敗",
          mapping.error());
      const std::wstring failure =
          L"選択したUSBとドライブ文字を一意に照合できないため、"
          L"安全側に停止しました。\n\n" +
          mapping.error().message +
          L"\n\nUSBの抜き差し後に「再読み込み」して再確認してください。";
      MessageBoxW(
          state.window,
          failure.c_str(),
          L"USBの安全照合で停止しました",
          MB_OK | MB_ICONWARNING);
      return;
    }
    if (mapping.value().drive_letter_was_unassigned) {
      message +=
          L"\n\n読み取り専用照合: ディスク " +
          std::to_wstring(
              mapping.value().target_identity.disk_number) +
          L"（全消去初期化予定）／作成時の割当予定: " +
          mapping.value().root_path;
    } else {
      message +=
          L"\n\n読み取り専用照合: ディスク " +
          std::to_wstring(
              mapping.value().target_identity.disk_number) +
          L" ↔ " + mapping.value().root_path +
          L"（パーティション " +
          std::to_wstring(mapping.value().partition_number) + L"）";
    }
    if (state.logger.has_value()) {
      state.logger->info(
          L"USB物理ディスクとドライブ文字の読み取り専用照合完了 disk=" +
          std::to_wstring(
              mapping.value().target_identity.disk_number) +
          L" drive_letter=" +
          std::wstring(1U, mapping.value().drive_letter) +
          L" physical_write_started=false");
    }
  }
  if (!plan.confirmation_token.empty()) {
    message +=
        L"\n\n実行時に入力する確認語:\n" +
        plan.confirmation_token;
  }
  message +=
      L"\n\nこの確認だけでは作成を開始しません。"
      L"\n続けて最終確認を表示します。";
  if (MessageBoxW(
          state.window,
          message.c_str(),
          L"レスキューメディアの作成内容",
          MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
    start_rescue_media_creation(state, plan);
  }
}

void create_restore_preflight_flow(AppState& state) {
  if (state.restore_preflight_running.load() ||
      state.restore_running.load()) {
    return;
  }

  std::vector<wchar_t> path(32U * 1024U, L'\0');
  constexpr wchar_t kFilter[] =
      L"Tsumugi Drive イメージ (*.tsumugi)\0*.tsumugi\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrTitle =
      L"完全検証する.tsumugiイメージを選択";
  dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                 OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                 OFN_DONTADDTORECENT;
  if (GetOpenFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      const std::wstring message =
          L"イメージ選択画面でエラーが発生しました。\n"
          L"Common dialog error: " +
          std::to_wstring(dialog_error);
      MessageBoxW(
          state.window,
          message.c_str(),
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  const std::wstring selected_path(path.data());
  std::vector<ytec::clonecore::StableDiskIdentity> protected_log_disks;
  auto image_backing_identity =
      identify_local_path_backing_read_only(selected_path);
  if (image_backing_identity) {
    protected_log_disks.push_back(image_backing_identity.take_value());
  }
  static_cast<void>(prepare_source_safe_product_logging(
      state, protected_log_disks, L"復元イメージの完全検証"));
  const auto header_probe =
      ytec::imageformat::probe_tsumugi_file_header_v1(selected_path);
  const auto initial_password_decision =
      ytec::windowsapp::evaluate_tsumugi_restore_password_prompt({
          .header_probe_succeeded = header_probe.has_value(),
          .encrypted = header_probe.has_value() &&
              header_probe.value().encrypted,
      });
  if (initial_password_decision ==
      ytec::windowsapp::TsumugiRestorePasswordPromptDecision::stop) {
    if (!header_probe.has_value()) {
      log_error_summary(
          state.logger,
          L"復元イメージの固定ヘッダー限定読取り失敗",
          header_probe.error());
      show_product_error(
          state.window,
          L"復元イメージを開けません",
          header_probe.error(),
          L"固定ヘッダーを確認できないため、完全検証も復元も開始していません。");
    }
    return;
  }

  std::shared_ptr<SecureAsciiPassword> password;
  if (initial_password_decision ==
      ytec::windowsapp::TsumugiRestorePasswordPromptDecision::
          prompt_required) {
    auto password_prompt = prompt_tsumugi_password(
        state.window,
        state.body_font,
        L"固定ヘッダーはこのイメージが暗号化済みであることを示しています。完全検証のためパスワードを入力してください。パスワードは検証完了または停止時にメモリから消去します。",
        L"暗号化イメージの完全検証",
        false,
        false,
        L"完全検証を開始");
    const auto completed_password_decision =
        ytec::windowsapp::evaluate_tsumugi_restore_password_prompt({
            .header_probe_succeeded = true,
            .encrypted = true,
            .prompt_accepted = password_prompt.accepted,
            .password_available = password_prompt.password != nullptr,
        });
    if (completed_password_decision !=
        ytec::windowsapp::TsumugiRestorePasswordPromptDecision::
            password_ready) {
      return;
    }
    password = std::move(password_prompt.password);
  }

  state.restore_preflight.reset();
  populate_restore_source_partition_candidates(state);
  state.restore_preflight_cancel_requested.store(false);
  state.restore_preflight_running.store(true);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);

  const HWND window = state.window;
  if (state.logger.has_value()) {
    state.logger->info(
        L"復元イメージの読み取り専用完全検証開始");
  }
  state.restore_preflight_thread = std::thread(
      [window, selected_path, password = std::move(password), &state]() {
        auto payload = std::make_unique<RestorePreflightPayload>();
        ytec::windowsapp::TsumugiRestoreImagePreflightOptions options{
            .cancellation_requested =
                [&state]() {
                  return state
                      .restore_preflight_cancel_requested.load();
                },
        };
        if (password != nullptr) {
          options.password = password->view();
        }
        auto result = ytec::windowsapp::inspect_tsumugi_restore_image_file(
            selected_path,
            options);
        if (result) {
          payload->report = result.take_value();
        } else {
          payload->error = result.error();
        }
        if (PostMessageW(
                window,
                kRestorePreflightCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void start_windows_data_rescue_clone_flow(AppState& state) {
  if (state.clone_running.load()) {
    return;
  }
  if (!state.elevated) {
    MessageBoxW(
        state.window,
        L"データディスク救出には、アプリ起動時の管理者権限が必要です。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto source_index = combo_selection(state.source_combo);
  const auto target_index = combo_selection(state.target_combo);
  if (!state.inventory.has_value() || !source_index.has_value() ||
      !target_index.has_value() ||
      source_index.value() >= state.inventory->disks.size() ||
      target_index.value() >= state.inventory->disks.size() ||
      source_index.value() == target_index.value()) {
    MessageBoxW(
        state.window,
        L"別々のコピー元とコピー先を選択してください。",
        L"救出対象を確認してください",
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& reviewed_source =
      state.inventory->disks[source_index.value()];
  const auto& reviewed_target =
      state.inventory->disks[target_index.value()];
  if (!require_healthy_write_target(
          state, reviewed_target, L"データディスク救出")) {
    return;
  }

  auto provider =
      ytec::diskmodel::make_windows_disk_inventory_provider(
          state.logger.has_value() ? &state.logger.value() : nullptr);
  if (!provider) {
    MessageBoxW(
        state.window,
        L"救出対象を再列挙するプロバイダーを作成できません。",
        L"救出を開始できません",
        MB_OK | MB_ICONERROR);
    return;
  }
  auto plan = ytec::windowsapp::prepare_windows_data_rescue_clone(
      reviewed_source.disk_number,
      reviewed_target.disk_number,
      *provider,
      ytec::windowsapp::
          query_windows_data_rescue_protected_target_with_windows_apis);
  if (!plan) {
    show_product_error(
        state.window,
        L"救出の安全確認で停止しました",
        plan.error());
    return;
  }

  const std::array<ytec::clonecore::StableDiskIdentity, 2U>
      protected_log_disks{
          plan.value().expected_source, plan.value().expected_target};
  if (!prepare_source_safe_product_logging(
          state, protected_log_disks, L"データディスク救出") ||
      !require_startup_write_access(state, L"データディスク救出") ||
      !confirm_long_operation_power(state, L"データディスク救出")) {
    return;
  }

  const std::wstring serial = reviewed_target.serial_suffix.empty()
      ? L"取得できません"
      : std::wstring(
            reviewed_target.serial_suffix.begin(),
            reviewed_target.serial_suffix.end());
  const std::wstring source_protection =
      plan.value().source_was_read_only
          ? L"read-only"
          : plan.value().source_was_offline ? L"offline" : L"未確認";
  const std::wstring health = std::wstring(
      ytec::diskmodel::disk_health_state_name(
          plan.value().source_health.state));
  const std::wstring first_confirmation =
      L"手順 1/2  データディスク救出の対象を確認してください。\n\n"
      L"コピー元: ディスク " +
      std::to_wstring(reviewed_source.disk_number) + L" / " +
      reviewed_source.model + L" / " +
      format_bytes(reviewed_source.size_bytes) +
      L"\nコピー元保護: " + source_protection +
      L" / 健康状態: " + health +
      L"\nコピー先: ディスク " +
      std::to_wstring(reviewed_target.disk_number) + L" / " +
      reviewed_target.model + L" / " +
      format_bytes(reviewed_target.size_bytes) +
      L"\nシリアル末尾: " + serial +
      L"\n\nコピー先の先頭からコピー元容量までをRAW上書きします。"
      L"読取不能範囲は前方・逆方向・sector単位で有限再試行し、残る欠損はゼロ埋めしてmapへ記録します。"
      L"\nコピー先が大きい場合、コピー元容量を超える末尾は消去も検証もせず、以前のデータが残る可能性があります。"
      L"\n縮小、GPT/MBR変換、起動修復は行いません。結果は必ず「一部欠損の可能性あり」と表示します。"
      L"\n\nこの対象で最終確認へ進みますか？";
  if (MessageBoxW(
          state.window,
          first_confirmation.c_str(),
          L"救出対象の安全確認 1/2",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  ConfirmationDialogState dialog_state{
      .details =
          L"救出元: ディスク " +
          std::to_wstring(reviewed_source.disk_number) + L" / " +
          reviewed_source.model +
          L"\r\n上書き先: ディスク " +
          std::to_wstring(reviewed_target.disk_number) + L" / " +
          reviewed_target.model +
          L"\r\n容量: " + format_bytes(reviewed_target.size_bytes) +
          L" / シリアル末尾: " + serial +
          L"\r\n上書き範囲: 先頭から " +
          format_bytes(reviewed_source.size_bytes) +
          L"（末尾余剰は未処理）"
          L"\r\n方式: RAW救出 / 欠損ゼロ埋めmap / 変換・起動修復なし",
      .token = L"OK",
      .confirm_button_label = L"救出コピーを開始",
      .font = state.body_font,
  };
  if (DialogBoxParamW(
          GetModuleHandleW(nullptr),
          MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
          state.window,
          confirmation_dialog_proc,
          reinterpret_cast<LPARAM>(&dialog_state)) != IDOK) {
    return;
  }

  state.clone_cancel_requested.store(false);
  state.clone_completion_post_failed.store(false);
  state.clone_progress.reset();
  state.clone_elapsed = std::chrono::milliseconds::zero();
  state.active_clone_is_rescue = true;
  state.active_clone_is_shrink = false;
  state.clone_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.clone_pause_controller;
  state.clone_running.store(true);
  const ULONGLONG started_tick = GetTickCount64();
  auto last_progress_tick =
      std::make_shared<std::atomic<ULONGLONG>>(0U);
  auto last_progress_stage = std::make_shared<std::atomic<std::uint8_t>>(
      static_cast<std::uint8_t>(0xFFU));
  const HWND window = state.window;
  ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress =
          [window,
           started_tick,
           last_progress_tick,
           last_progress_stage](
              const ytec::clonecore::DiskOperationProgress& progress) {
            const ULONGLONG now = GetTickCount64();
            const auto stage = static_cast<std::uint8_t>(progress.stage);
            const bool stage_changed =
                last_progress_stage->exchange(stage) != stage;
            const ULONGLONG previous = last_progress_tick->load();
            if (!stage_changed && now - previous < 100U) {
              return;
            }
            last_progress_tick->store(now);
            auto payload = std::make_unique<CloneProgressPayload>();
            payload->progress = progress;
            payload->elapsed =
                std::chrono::milliseconds(now - started_tick);
            if (PostMessageW(
                    window,
                    kCloneProgressMessage,
                    0,
                    reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
              static_cast<void>(payload.release());
            }
          },
      .cancellation_requested = [&state]() {
        return state.clone_cancel_requested.load();
      },
  };
  callbacks = ytec::clonecore::bind_manual_pause_controller(
      std::move(callbacks), pause_controller);
  auto dependencies =
      ytec::windowsapp::make_windows_data_rescue_clone_dependencies(
          ytec::windowsapp::
              query_windows_data_rescue_protected_target_with_windows_apis);
  const std::uint32_t target_disk_number =
      plan.value().expected_target.disk_number;
  if (state.logger.has_value()) {
    state.logger->warning(
        L"Windowsデータディスク救出開始 source_disk=" +
        std::to_wstring(plan.value().expected_source.disk_number) +
        L" target_disk=" + std::to_wstring(target_disk_number) +
        L" classification=partial-loss-possible");
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const std::uint64_t completion_power_operation_binding =
      ytec::windowsapp::take_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  state.clone_thread = std::thread(
      [window,
       reviewed_plan = plan.take_value(),
       dependencies = std::move(dependencies),
       callbacks = std::move(callbacks),
       target_disk_number,
        pause_controller,
        completion_power_operation_binding,
        completion_post_failed = &state.clone_completion_post_failed,
       clone_running = &state.clone_running]() mutable {
        auto payload = std::make_unique<ClonePayload>();
        payload->target_disk_number = target_disk_number;
        payload->rescue_mode = true;
        payload->completion_power_operation_binding =
            completion_power_operation_binding;
        ThreadSleepPrevention sleep_prevention;
        if (!sleep_prevention.active()) {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"データ救出中の自動スリープ防止",
              .message = L"自動スリープを安全に防止できないため開始しません",
          };
        } else {
          auto result =
              ytec::windowsapp::execute_windows_data_rescue_clone(
                  reviewed_plan,
                  true,
                  L"OK",
                  dependencies,
                  std::move(callbacks));
          if (result) {
            payload->rescue_report = result.take_value();
          } else {
           payload->error = result.error();
          }
        }
        pause_controller->mark_completed();
        payload->sleep_prevention_release = sleep_prevention.release();
        bool posted = false;
        for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
          if (PostMessageW(
                  window,
                  kCloneCompleteMessage,
                  0,
                  reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
            static_cast<void>(payload.release());
            posted = true;
            break;
          }
          if (IsWindow(window) == FALSE) {
            break;
          }
          Sleep(10U);
        }
        if (!posted) {
          completion_post_failed->store(true);
          clone_running->store(false);
          static_cast<void>(SetTimer(
              window,
              kCloneCompletionFallbackTimerId,
              50U,
              nullptr));
        }
      });
}

void start_windows_direct_shrink_clone_flow(
    AppState& state,
    const ytec::diskmodel::DiskInfo& source,
    const ytec::diskmodel::DiskInfo& target) {
  if (!require_healthy_write_target(
          state, target, L"縮小移行クローン") ||
      !confirm_source_health_advice(
          state, source, L"縮小移行クローン")) {
    return;
  }

  const auto source_identity =
      ytec::diskmodel::make_stable_disk_identity(
          source, source.is_system_disk);
  const auto target_identity =
      ytec::diskmodel::make_stable_disk_identity(target, false);
  if (!source_identity || !target_identity) {
    show_product_error(
        state.window,
        L"ディスク識別情報が不足しています",
        source_identity ? target_identity.error() : source_identity.error());
    return;
  }
  const std::array<ytec::clonecore::StableDiskIdentity, 2U>
      protected_log_disks{
          source_identity.value(), target_identity.value()};
  if (!prepare_source_safe_product_logging(
          state, protected_log_disks, L"縮小移行クローン") ||
      !require_startup_write_access(state, L"縮小移行クローン")) {
    return;
  }

  const auto version = current_windows_version();
  const std::string architecture = current_native_architecture();
  const std::string analysis_created_utc = current_utc_timestamp();
  if (!version || architecture.empty() || analysis_created_utc.empty()) {
    if (!version) {
      show_product_error(
          state.window,
          L"Windowsの環境を確認できません",
          version.error());
    } else {
      MessageBoxW(
          state.window,
          architecture.empty()
              ? L"AMD64版Windowsとして安全に識別できませんでした。"
              : L"縮小解析時刻を安全に確定できませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }
  auto operation_id = ytec::windowsapp::
      make_online_direct_clone_operation_id_with_windows_apis();
  if (!operation_id) {
    show_product_error(
        state.window,
        L"縮小移行の操作IDを作成できません",
        operation_id.error());
    return;
  }

  auto plan = ytec::windowsapp::
      plan_windows_direct_shrink_clone_with_windows_apis(
          ytec::windowsapp::WindowsDirectShrinkProductPlanningRequest{
              .administrator = state.elevated,
              .target_is_active_rescue_media = false,
              .reviewed_source = source,
              .reviewed_target = target,
              .operation_id = operation_id.take_value(),
              .surplus_allocation = ytec::migrationcore::
                  ShrinkSurplusAllocation::automatic_proportional,
              .windows_major = version.value()[0],
              .windows_minor = version.value()[1],
              .windows_build = version.value()[2],
              .windows_architecture = architecture,
              .analysis_created_utc = analysis_created_utc,
              .app_version = std::string(kAppVersion),
          });
  if (!plan) {
    show_product_error(
        state.window,
        L"縮小移行の安全計画を作成できません",
        plan.error(),
        L"コピー先への書き込み、VSS作成、対象ディスクの変更は開始していません。稼働中Windowsとアプリの通常ログ書き込みは別です。MBR維持とMBRからGPTへの変換はWindows版の直接縮小では扱いません。");
    return;
  }
  if (!confirm_long_operation_power(state, L"縮小移行クローン")) {
    return;
  }

  const auto serial_text = [](const ytec::diskmodel::DiskInfo& disk) {
    return disk.serial_suffix.empty()
        ? std::wstring(L"取得できません")
        : std::wstring(
              disk.serial_suffix.begin(), disk.serial_suffix.end());
  };
  const std::wstring first_confirmation =
      L"手順 1/2  コピー元と消去対象を確認してください。\n\n"
      L"コピー元: ディスク " +
      std::to_wstring(source.disk_number) + L" / " + source.model +
      L" / " + format_bytes(source.size_bytes) +
      L" / シリアル末尾: " + serial_text(source) +
      L"\nコピー先: ディスク " +
      std::to_wstring(target.disk_number) + L" / " + target.model +
      L" / " + format_bytes(target.size_bytes) +
      L" / シリアル末尾: " + serial_text(target) +
      L"\n削除対象: コピー先ディスク全体（" +
      std::to_wstring(target.partitions.size()) +
      L" パーティションと全データ）"
      L"\n作成構成: GPT維持 / " +
      std::to_wstring(plan.value().tasks().size()) +
      L" パーティション / NTFS取込 " +
      std::to_wstring(plan.value().archive_task_count()) +
      L" 件"
      L"\n一時領域: コピー先内部の専用NTFS（上限 " +
      format_bytes(plan.value().staging().archive_capacity_bytes) +
      L"）"
      L"\n余剰容量: 検証後にNTFSへ自動配分"
      L"\n完了後: コピー先はオフラインのまま保持"
      L"\n\nVSS Snapshotから1領域ずつ一時WIMへ取り込み、"
      L"読戻し検証後にだけ最終GPTを公開します。"
      L"一時WIMが専用領域に収まらない場合はコピー先を未完成・"
      L"オフラインにして安全に中止します。"
      L"\nアプリはコピー元へ書き込みませんが、稼働中Windowsと"
      L"VSSの通常書き込みは発生します。"
      L"\n\nこの対象で最終確認へ進みますか？";
  if (MessageBoxW(
          state.window,
          first_confirmation.c_str(),
          L"縮小移行の安全確認 1/2",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  ConfirmationDialogState dialog_state{
      .details =
          L"作成先: ディスク " +
          std::to_wstring(target.disk_number) + L" / " + target.model +
          L"\r\n容量: " + format_bytes(target.size_bytes) +
          L" / シリアル末尾: " + serial_text(target) +
          L"\r\n削除対象: ディスク全体（既存パーティションを含む全内容）"
          L"\r\n作成構成: GPT維持 / Windows・回復NTFSを縮小再構成"
          L"\r\n一時領域: コピー先所有 / 容量超過時は未完成のまま停止"
          L"\r\n完了状態: 全読戻し検証後もオフライン / 実機起動は未証明",
      .token = L"OK",
      .confirm_button_label = L"縮小移行を開始",
      .font = state.body_font,
  };
  const INT_PTR dialog_result = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
      state.window,
      confirmation_dialog_proc,
      reinterpret_cast<LPARAM>(&dialog_state));
  if (dialog_result != IDOK) {
    return;
  }

  state.clone_cancel_requested.store(false);
  state.clone_completion_post_failed.store(false);
  state.clone_progress.reset();
  state.clone_elapsed = std::chrono::milliseconds::zero();
  state.active_clone_is_rescue = false;
  state.active_clone_is_shrink = true;
  state.clone_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.clone_pause_controller;
  state.clone_running.store(true);
  const ULONGLONG started_tick = GetTickCount64();
  auto last_progress_tick =
      std::make_shared<std::atomic<ULONGLONG>>(0U);
  auto last_progress_stage = std::make_shared<std::atomic<std::uint8_t>>(
      static_cast<std::uint8_t>(0xFFU));
  const HWND window = state.window;
  ytec::windowsapp::WindowsDirectShrinkCloneExecutionOptions options{
      .async_wait = ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 120'000U,
          .poll_interval_ms = 250U,
          .cancellation_requested = [&state]() {
            return state.clone_cancel_requested.load();
          },
      },
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
      .callbacks = ytec::clonecore::bind_manual_pause_controller(
          ytec::clonecore::DiskOperationCallbacks{
              .progress =
                  [window,
                   started_tick,
                   last_progress_tick,
                   last_progress_stage](
                      const ytec::clonecore::DiskOperationProgress&
                          progress) {
                    const ULONGLONG now = GetTickCount64();
                    const auto stage =
                        static_cast<std::uint8_t>(progress.stage);
                    const bool stage_changed =
                        last_progress_stage->exchange(stage) != stage;
                    const ULONGLONG previous = last_progress_tick->load();
                    if (!stage_changed && now - previous < 100U) {
                      return;
                    }
                    last_progress_tick->store(now);
                    auto payload =
                        std::make_unique<CloneProgressPayload>();
                    payload->progress = progress;
                    payload->elapsed =
                        std::chrono::milliseconds(now - started_tick);
                    if (PostMessageW(
                            window,
                            kCloneProgressMessage,
                            0,
                            reinterpret_cast<LPARAM>(payload.get())) !=
                        FALSE) {
                      static_cast<void>(payload.release());
                    }
                  },
              .cancellation_requested = [&state]() {
                return state.clone_cancel_requested.load();
              },
          },
          pause_controller),
      .logger = state.logger.has_value() ? &state.logger.value() : nullptr,
  };
  if (state.logger.has_value()) {
    state.logger->info(
        L"Windows直接縮小クローン開始 source_disk=" +
        std::to_wstring(source.disk_number) + L" target_disk=" +
        std::to_wstring(target.disk_number) + L" tasks=" +
        std::to_wstring(plan.value().tasks().size()) +
        L" target_owned_staging_bytes=" +
        std::to_wstring(plan.value().staging().length_bytes));
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const std::uint64_t completion_power_operation_binding =
      ytec::windowsapp::take_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  state.clone_thread = std::thread(
      [window,
       reviewed_plan = plan.take_value(),
       options = std::move(options),
       target_disk_number = target.disk_number,
       pause_controller,
       completion_post_failed = &state.clone_completion_post_failed,
       clone_running = &state.clone_running,
       completion_power_operation_binding]() mutable {
        auto payload = std::make_unique<ClonePayload>();
        payload->target_disk_number = target_disk_number;
        payload->shrink_mode = true;
        payload->completion_power_operation_binding =
            completion_power_operation_binding;
        ThreadSleepPrevention sleep_prevention;
        if (!sleep_prevention.active()) {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"縮小移行中の自動スリープ防止",
              .message = L"自動スリープを安全に防止できないため開始しません",
          };
        } else {
          auto result = ytec::windowsapp::
              execute_windows_direct_shrink_clone_with_windows_apis(
                  reviewed_plan, options);
          if (result) {
            payload->shrink_report = result.take_value();
          } else {
            payload->error = result.error();
          }
        }
        pause_controller->mark_completed();
        payload->sleep_prevention_release = sleep_prevention.release();
        bool posted = false;
        for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
          if (PostMessageW(
                  window,
                  kCloneCompleteMessage,
                  0,
                  reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
            static_cast<void>(payload.release());
            posted = true;
            break;
          }
          if (IsWindow(window) == FALSE) {
            break;
          }
          Sleep(10U);
        }
        if (!posted) {
          completion_post_failed->store(true);
          clone_running->store(false);
          static_cast<void>(SetTimer(
              window,
              kCloneCompletionFallbackTimerId,
              50U,
              nullptr));
        }
      });
}


void start_online_direct_clone_flow(AppState& state) {
  if (state.clone_running.load()) {
    return;
  }
  if (selected_clone_rescue_mode(state)) {
    start_windows_data_rescue_clone_flow(state);
    return;
  }
  if (!state.elevated) {
    MessageBoxW(
        state.window,
        L"ドライブのクローンは、アプリ起動時の管理者権限が必要です。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto source_index = combo_selection(state.source_combo);
  const auto target_index = combo_selection(state.target_combo);
  if (!state.inventory.has_value() ||
      !source_index.has_value() || !target_index.has_value() ||
      source_index.value() >= state.inventory->disks.size() ||
      target_index.value() >= state.inventory->disks.size()) {
    MessageBoxW(
        state.window,
        L"コピー元とコピー先を再度確認してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto selection = current_clone_selection(state);
  if (!selection.ready) {
    MessageBoxW(
        state.window,
        selection.message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& source = state.inventory->disks[source_index.value()];
  const auto& target = state.inventory->disks[target_index.value()];
  if (selected_transfer_mode(state) ==
      ytec::imageformat::TransferMode::shrink) {
    start_windows_direct_shrink_clone_flow(state, source, target);
    return;
  }
  if (!require_healthy_write_target(
          state, target, L"ドライブのクローン") ||
      !confirm_source_health_advice(
          state, source, L"ドライブのクローン")) {
    return;
  }
  const auto source_identity =
      ytec::diskmodel::make_stable_disk_identity(
          source, source.is_system_disk);
  const auto target_identity =
      ytec::diskmodel::make_stable_disk_identity(target, false);
  if (!source_identity || !target_identity) {
    const auto& error = source_identity
        ? target_identity.error()
        : source_identity.error();
    show_product_error(
        state.window,
        L"ディスク識別情報が不足しています",
        error);
    return;
  }
  const std::array<ytec::clonecore::StableDiskIdentity, 2U>
      protected_log_disks{
          source_identity.value(), target_identity.value()};
  if (!prepare_source_safe_product_logging(
          state, protected_log_disks, L"ドライブのクローン") ||
      !require_startup_write_access(state, L"ドライブのクローン")) {
    return;
  }
  auto source_layout_hash = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(source);
  auto target_layout_hash = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(target);
  if (!source_layout_hash || !target_layout_hash) {
    const auto& error = source_layout_hash
        ? target_layout_hash.error()
        : source_layout_hash.error();
    show_product_error(
        state.window,
        L"ディスク構成を確定できません",
        error);
    return;
  }
  if (!confirm_long_operation_power(state, L"ドライブのクローン")) {
    return;
  }

  const std::wstring serial = target.serial_suffix.empty()
      ? L"取得できません"
      : std::wstring(target.serial_suffix.begin(), target.serial_suffix.end());
  const std::wstring first_confirmation =
      L"手順 1/2  コピー元と消去対象を確認してください。\n\n"
      L"コピー元: ディスク " +
      std::to_wstring(source.disk_number) + L" / " + source.model +
      L" / " + format_bytes(source.size_bytes) +
      L"\nコピー先: ディスク " +
      std::to_wstring(target.disk_number) + L" / " + target.model +
      L" / " + format_bytes(target.size_bytes) +
      L"\nシリアル末尾: " + serial +
      L"\n削除対象: コピー先ディスク全体（" +
      std::to_wstring(target.partitions.size()) +
      L" パーティションと全データ）"
      L"\n完了後: コピー先はオフラインのまま保持"
      L"\n\nVSS Snapshotで開始時点のNTFS領域をコピーします。"
      L"アプリはコピー元へ書き込みませんが、稼働中WindowsとVSSの通常書き込みは発生します。"
      L"\n\nこの対象で最終確認へ進みますか？";
  if (MessageBoxW(
          state.window,
          first_confirmation.c_str(),
          L"コピー先の安全確認 1/2",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  ConfirmationDialogState dialog_state{
      .details =
          L"作成先: ディスク " +
          std::to_wstring(target.disk_number) + L" / " + target.model +
          L"\r\n容量: " + format_bytes(target.size_bytes) +
          L" / シリアル末尾: " + serial +
          L"\r\n削除対象: ディスク全体（既存パーティションを含む全内容）"
          L"\r\n作成構成: " + partition_style_text(source.partition_style) +
          L" 維持" +
          (source.is_system_disk
               ? L" / 起動情報は新規再構築"
               : L" / データディスク"),
      .token = L"OK",
      .confirm_button_label = L"クローンを開始",
      .font = state.body_font,
  };
  const INT_PTR dialog_result = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
      state.window,
      confirmation_dialog_proc,
      reinterpret_cast<LPARAM>(&dialog_state));
  if (dialog_result != IDOK) {
    return;
  }

  auto operation_id = ytec::windowsapp::
      make_online_direct_clone_operation_id_with_windows_apis();
  if (!operation_id) {
    show_product_error(
        state.window,
        L"クローン操作を開始できません",
        operation_id.error());
    return;
  }

  state.clone_cancel_requested.store(false);
  state.clone_completion_post_failed.store(false);
  state.clone_progress.reset();
  state.clone_elapsed = std::chrono::milliseconds::zero();
  state.active_clone_is_rescue = false;
  state.active_clone_is_shrink = false;
  state.clone_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.clone_pause_controller;
  state.clone_running.store(true);
  const ULONGLONG started_tick = GetTickCount64();
  auto last_progress_tick =
      std::make_shared<std::atomic<ULONGLONG>>(0U);
  auto last_progress_stage = std::make_shared<std::atomic<std::uint8_t>>(
      static_cast<std::uint8_t>(0xFFU));
  const HWND window = state.window;
  ytec::windowsapp::OnlineDirectCloneRequest request{
      .administrator = true,
      .expected_source = source_identity.value(),
      .expected_target = target_identity.value(),
      .expected_source_layout_hash = source_layout_hash.take_value(),
      .expected_target_layout_hash = target_layout_hash.take_value(),
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
      .maximum_chunk_bytes = 4U * 1024U * 1024U,
      .async_wait = ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 120'000U,
          .poll_interval_ms = 250U,
          .cancellation_requested = [&state]() {
            return state.clone_cancel_requested.load();
          },
      },
      .callbacks = ytec::clonecore::bind_manual_pause_controller(
          ytec::clonecore::DiskOperationCallbacks{
          .progress =
              [window,
               started_tick,
               last_progress_tick,
               last_progress_stage](
                  const ytec::clonecore::DiskOperationProgress& progress) {
                const ULONGLONG now = GetTickCount64();
                const auto stage = static_cast<std::uint8_t>(progress.stage);
                const bool stage_changed =
                    last_progress_stage->exchange(stage) != stage;
                const ULONGLONG previous = last_progress_tick->load();
                if (!stage_changed && now - previous < 100U) {
                  return;
                }
                last_progress_tick->store(now);
                auto payload = std::make_unique<CloneProgressPayload>();
                payload->progress = progress;
                payload->elapsed =
                    std::chrono::milliseconds(now - started_tick);
                if (PostMessageW(
                        window,
                        kCloneProgressMessage,
                        0,
                        reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
                  static_cast<void>(payload.release());
                }
              },
          .cancellation_requested = [&state]() {
            return state.clone_cancel_requested.load();
          },
          },
          pause_controller),
      .logger = state.logger.has_value() ? &state.logger.value() : nullptr,
  };
  ytec::windowsapp::OnlineDirectCloneOperationRequest operation_request{
      .reviewed_source = source,
      .reviewed_target = target,
      .clone = std::move(request),
      .operation_id = operation_id.take_value(),
  };
  if (state.logger.has_value()) {
    state.logger->info(
        L"Windows直接クローン開始 source_disk=" +
        std::to_wstring(source.disk_number) + L" target_disk=" +
        std::to_wstring(target.disk_number));
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const std::uint64_t completion_power_operation_binding =
      ytec::windowsapp::take_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  state.clone_thread = std::thread(
      [window,
       request = std::move(operation_request),
       target_disk_number = target.disk_number,
       pause_controller,
       completion_power_operation_binding,
       completion_post_failed =
           &state.clone_completion_post_failed,
       clone_running = &state.clone_running]() {
        auto payload = std::make_unique<ClonePayload>();
        payload->target_disk_number = target_disk_number;
        payload->completion_power_operation_binding =
            completion_power_operation_binding;
        ThreadSleepPrevention sleep_prevention;
        if (!sleep_prevention.active()) {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"クローン中の自動スリープ防止",
              .message = L"自動スリープを安全に防止できないため開始しません",
          };
        } else {
          auto result = ytec::windowsapp::
              execute_online_direct_clone_operation_with_windows_apis(
                  request);
          if (result) {
            payload->report = result.take_value();
          } else {
            payload->error = result.error();
          }
        }
        pause_controller->mark_completed();
        payload->sleep_prevention_release = sleep_prevention.release();
        bool posted = false;
        for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
          if (PostMessageW(
                  window,
                  kCloneCompleteMessage,
                  0,
                  reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
            static_cast<void>(payload.release());
            posted = true;
            break;
          }
          if (IsWindow(window) == FALSE) {
            break;
          }
          Sleep(10U);
        }
        if (!posted) {
          completion_post_failed->store(true);
          clone_running->store(false);
          static_cast<void>(SetTimer(
              window,
              kCloneCompletionFallbackTimerId,
              50U,
              nullptr));
        }
      });
}

void start_online_image_restore_flow(AppState& state) {
  if (state.restore_running.load()) {
    return;
  }
  if (!state.elevated) {
    MessageBoxW(
        state.window,
        L"イメージの復元は、アプリ起動時の管理者権限が必要です。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  if (!state.restore_preflight.has_value() ||
      !state.inventory.has_value()) {
    return;
  }
  const auto target_index = combo_selection(state.target_combo);
  const auto selection = current_restore_target_selection(state);
  if (!target_index.has_value() ||
      target_index.value() >= state.inventory->disks.size() ||
      !selection.ready_for_confirmation ||
      !selection.target_identity.has_value()) {
    MessageBoxW(
        state.window,
        L"完全検証済みイメージと、安全確認可能な復元先を選択してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& image = state.restore_preflight.value();
  const auto& target = state.inventory->disks[target_index.value()];
  const auto individual_partition =
      current_restore_individual_partition_selection(state);
  if (!require_healthy_write_target(
          state, target, L"イメージからの復元")) {
    return;
  }
  const bool shrink_restore = image.manifest.mode ==
      ytec::imageformat::TsumugiManifestMode::shrink;
  auto layout_hash =
      ytec::windowsapp::hash_online_image_restore_target_layout(target);
  if (!layout_hash) {
    show_product_error(
        state.window,
        L"復元先の構成を確定できません",
        layout_hash.error());
    return;
  }
  std::vector<ytec::clonecore::StableDiskIdentity> protected_log_disks;
  auto image_backing_identity =
      identify_local_path_backing_read_only(image.canonical_path);
  if (!image_backing_identity) {
    show_product_error(
        state.window,
        L"復元イメージの保存ディスクを識別できません",
        image_backing_identity.error());
    return;
  }
  const auto storage_separation = ytec::windowsapp::
      validate_tsumugi_restore_storage_target_separation(
          image_backing_identity.value(),
          selection.target_identity.value());
  if (!storage_separation) {
    show_product_error(
        state.window,
        L"復元イメージの保存ディスクは選択できません",
        storage_separation.error());
    return;
  }
  protected_log_disks.push_back(image_backing_identity.take_value());
  protected_log_disks.push_back(selection.target_identity.value());
  if (!prepare_source_safe_product_logging(
          state, protected_log_disks, L"イメージからの復元") ||
      !require_startup_write_access(state, L"イメージからの復元")) {
    return;
  }
  std::optional<ytec::windowsapp::WindowsShrinkWorkPaths>
      shrink_work_paths;
  std::optional<
      ytec::windowsapp::WindowsShrinkWorkPlacementObservation>
      shrink_work_observation;
  std::optional<
      ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
      shrink_layout;
  if (shrink_restore) {
    if (!state.startup_data_policy.persistent_logging_permitted() ||
        state.startup_data_policy.data_directory.empty() ||
        state.log_path.empty() ||
        protected_log_disks.size() != 2U) {
      MessageBoxW(
          state.window,
          L"縮小復元では、WIM staging・checkpoint・logを、イメージ保存ディスクと復元先のどちらでもない第三のローカルディスクへ置く必要があります。\n\n"
          L"現在のEXE隣dataを安全な第三ディスクとして証明できないため開始しません。アプリを第三のディスクへ配置して再実行してください。",
          L"縮小復元の作業場所が不足しています",
          MB_OK | MB_ICONWARNING);
      return;
    }
    const std::filesystem::path data_directory(
        state.startup_data_policy.data_directory);
    shrink_work_paths = ytec::windowsapp::WindowsShrinkWorkPaths{
        .scratch_directory = data_directory.wstring(),
        .checkpoint_path =
            (data_directory / L"active.checkpoint").wstring(),
        .log_path = state.log_path,
        .log_is_ram_only = false,
    };
    auto observed = ytec::windowsapp::
        observe_windows_shrink_work_placement_with_windows_apis(
            shrink_work_paths.value());
    if (!observed) {
      show_product_error(
          state.window,
          L"縮小復元の作業場所を確定できません",
          observed.error());
      return;
    }
    shrink_work_observation = observed.take_value();
    auto planned = ytec::windowsapp::
        make_windows_online_shrink_restore_layout_with_windows_apis(
            image, target, selection.target_identity.value());
    if (!planned) {
      show_product_error(
          state.window,
          L"縮小復元の最終配置を作成できません",
          planned.error());
      return;
    }
    shrink_layout = planned.take_value();
  }
  if (!confirm_long_operation_power(state, L"イメージからの復元")) {
    return;
  }
  const std::wstring serial =
      target.serial_suffix.empty()
          ? L"取得できません"
          : std::wstring(
                target.serial_suffix.begin(),
                target.serial_suffix.end());
  std::wstring partition_details =
      L"\n現在の構成: " + partition_style_text(target.partition_style) +
      L" / " + std::to_wstring(target.partitions.size()) + L"区画";
  constexpr std::size_t kMaximumVisiblePartitions = 6U;
  const std::size_t visible = (std::min)(
      target.partitions.size(), kMaximumVisiblePartitions);
  for (std::size_t index = 0U; index < visible; ++index) {
    const auto& partition = target.partitions[index];
    partition_details +=
        L"\n  [" + std::to_wstring(partition.number) + L"] " +
        format_bytes(partition.size_bytes) + L"  " +
        (partition.name.empty() ? partition.type : partition.name);
  }
  if (target.partitions.size() > visible) {
    partition_details +=
        L"\n  ほか " +
        std::to_wstring(target.partitions.size() - visible) + L"区画";
  }
  std::wstring shrink_details;
  if (shrink_restore && shrink_layout.has_value()) {
    shrink_details =
        L"\n最終構成: " +
        std::wstring(
            shrink_layout->metadata.style ==
                    ytec::imageformat::PartitionTableStyle::gpt
                ? L"GPT"
                : L"MBR") +
        L" / " +
        std::to_wstring(
            shrink_layout->migration.target_partitions.size()) +
        L"区画 / 未割当末尾 " +
        format_bytes(
            shrink_layout->migration.unallocated_tail_bytes) +
        L"\n処理方式: 一時的な非起動GPT区画を1件ずつ作成し、WIM適用・全ファイル読戻し後に退役。最終パーティション表は最後に確定";
  }
  std::wstring individual_details;
  bool individual_unallocated = false;
  if (individual_partition.has_value()) {
    const auto* existing = std::get_if<ytec::imageformat::
        TsumugiPhysicalExistingPartitionRestoreSelection>(
        &individual_partition->target);
    const auto* unallocated = std::get_if<ytec::imageformat::
        TsumugiPhysicalUnallocatedRestoreSelection>(
        &individual_partition->target);
    const auto source = std::find_if(
        image.manifest.partitions.begin(),
        image.manifest.partitions.end(),
        [&](const ytec::imageformat::TsumugiManifestPartition& partition) {
          return partition.source_table_index ==
              individual_partition->source_table_index;
        });
    if ((existing == nullptr) == (unallocated == nullptr) ||
        source == image.manifest.partitions.end()) {
      MessageBoxW(
          state.window,
          L"レビュー済みの個別パーティション配置を再構成できません。ディスクは開いていません。",
          L"個別復元を開始できません",
          MB_OK | MB_ICONERROR);
      return;
    }
    individual_unallocated = unallocated != nullptr;
    individual_details =
        L"\n復元範囲: 画像内の区画 " +
        std::to_wstring(source->source_partition_number) + L"（" +
        format_bytes(source->source_size) + L"）";
    if (existing != nullptr) {
      individual_details +=
          L"\n上書き先: 既存区画 " +
          std::to_wstring(existing->target_partition_number) + L"（" +
          format_bytes(existing->target_size) + L"）" +
          L"\nパーティション表: 変更しない";
    } else {
      individual_details +=
          L"\n作成先: 未割当 offset " +
          format_bytes(unallocated->target_offset) + L" / 新規区画 " +
          format_bytes(unallocated->target_size) +
          L"\nパーティション表: 既存entryを保持し、新規1entryだけを最後に確定";
    }
  }
  const std::wstring deletion_details = individual_partition.has_value()
      ? individual_unallocated
            ? L"\n\n書込み対象: 選択した未割当範囲（自動バックアップなし）\n"
              L"既存区画と既存entryは保持し、新規1区画だけを追加します。\n"
            : L"\n\n上書き対象: 選択した既存パーティションの内容だけ（自動バックアップなし）\n"
              L"他の区画とパーティション表は変更しません。\n"
      : L"\n\n削除対象: このディスクのパーティション表と全内容\n";
  const std::wstring first_review =
      L"選択した .tsumugi は全チャンクと全体Hashの完全検証に合格しています。\n\n"
      L"復元先: ディスク " + std::to_wstring(target.disk_number) +
      L" / " + target.model +
       L"\n容量: " + format_bytes(target.size_bytes) +
       L" / シリアル末尾: " + serial +
       partition_details + shrink_details + individual_details +
       deletion_details +
       L"復元後: 読戻し検証を完了し、ディスクはオフラインのまま保持\n\n"
      L"この復元先で間違いありませんか？";
  if (MessageBoxW(
          state.window,
          first_review.c_str(),
          L"復元先の最終確認 1/2",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }

  ConfirmationDialogState dialog_state{
      .details =
          L"復元先: ディスク " + std::to_wstring(target.disk_number) +
          L" / " + target.model +
          L"\r\n容量: " + format_bytes(target.size_bytes) +
          L" / シリアル末尾: " + serial +
          L"\r\n現在の構成: " +
          partition_style_text(target.partition_style) + L" / " +
          std::to_wstring(target.partitions.size()) + L"区画" +
          (shrink_restore && shrink_layout.has_value()
               ? L"\r\n最終構成: " +
                     std::wstring(
                         shrink_layout->metadata.style ==
                                 ytec::imageformat::PartitionTableStyle::gpt
                             ? L"GPT"
                             : L"MBR") +
                     L" / " +
                     std::to_wstring(
                         shrink_layout->migration.target_partitions.size()) +
                     L"区画"
               : std::wstring{}) +
           (individual_partition.has_value()
                ? individual_unallocated
                      ? L"\r\n書込み対象: 選択した未割当範囲、新規1区画を追加（自動バックアップなし）"
                      : L"\r\n上書き対象: 選択した既存パーティションの内容だけ（自動バックアップなし）"
                : L"\r\n削除対象: 復元先ディスク全体（既存の全内容）"),
      .token = L"OK",
      .confirm_button_label = L"復元を開始",
      .font = state.body_font,
  };
  const INT_PTR dialog_result = DialogBoxParamW(
      GetModuleHandleW(nullptr),
      MAKEINTRESOURCEW(IDD_CLONE_CONFIRMATION),
      state.window,
      confirmation_dialog_proc,
      reinterpret_cast<LPARAM>(&dialog_state));
  if (dialog_result != IDOK) {
    if (dialog_result == -1) {
      MessageBoxW(
          state.window,
          L"復元先の最終確認画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  std::shared_ptr<SecureAsciiPassword> restore_password;
  if (image.encrypted) {
    auto password_prompt = prompt_tsumugi_password(
        state.window,
        state.body_font,
        L"暗号化イメージを復元するため、パスワードをもう一度入力してください。復元処理の完了または停止時にメモリから消去します。",
        L"暗号化イメージの復元",
        false,
        false,
        L"復元を開始");
    if (!password_prompt.accepted || password_prompt.password == nullptr) {
      return;
    }
    restore_password = std::move(password_prompt.password);
  }

  auto operation_id = ytec::windowsapp::
      make_online_image_restore_operation_id_with_windows_apis();
  if (!operation_id) {
    show_product_error(
        state.window,
        L"復元操作を開始できません",
        operation_id.error());
    return;
  }

  state.restore_cancel_requested.store(false);
  state.restore_progress.reset();
  state.restore_elapsed = std::chrono::milliseconds::zero();
  state.restore_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.restore_pause_controller;
  state.restore_running.store(true);
  const ULONGLONG started_tick = GetTickCount64();
  auto last_progress_tick =
      std::make_shared<std::atomic<ULONGLONG>>(0U);
  auto last_progress_stage = std::make_shared<std::atomic<std::uint8_t>>(
      static_cast<std::uint8_t>(0xFFU));
  const HWND window = state.window;
  ytec::clonecore::DiskOperationCallbacks restore_callbacks{
      .progress =
          [window,
           started_tick,
           last_progress_tick,
           last_progress_stage](
              const ytec::clonecore::DiskOperationProgress& progress) {
            const ULONGLONG now = GetTickCount64();
            const auto stage = static_cast<std::uint8_t>(progress.stage);
            const bool stage_changed =
                last_progress_stage->exchange(stage) != stage;
            const ULONGLONG previous = last_progress_tick->load();
            if (!stage_changed && now - previous < 100U) {
              return;
            }
            last_progress_tick->store(now);
            auto payload = std::make_unique<RestoreProgressPayload>();
            payload->progress = progress;
            payload->elapsed =
                std::chrono::milliseconds(now - started_tick);
            if (PostMessageW(
                    window,
                    kRestoreProgressMessage,
                    0,
                    reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
              static_cast<void>(payload.release());
            }
          },
      .cancellation_requested = [&state]() {
        return state.restore_cancel_requested.load();
      },
  };
  restore_callbacks = ytec::clonecore::bind_manual_pause_controller(
      std::move(restore_callbacks), pause_controller);
  ytec::imageformat::TsumugiImageVerifyRequest verify_request{
      .image_path = image.canonical_path,
      .storage_file_system = image.storage_file_system,
      .password = restore_password != nullptr
          ? std::optional<std::string_view>(restore_password->view())
          : std::nullopt,
  };
  const auto target_identity = selection.target_identity.value();
  const auto target_layout_hash = layout_hash.value();
  const auto restore_operation_id = operation_id.take_value();
  std::optional<ytec::windowsapp::OnlineImageRestoreOperationRequest>
      exact_request;
  std::optional<
      ytec::windowsapp::WindowsOnlineShrinkRestoreRequest>
      shrink_request;
  if (shrink_restore) {
    if (!shrink_work_paths.has_value() ||
        !shrink_work_observation.has_value() ||
        !shrink_layout.has_value() || protected_log_disks.empty()) {
      state.restore_running.store(false);
      pause_controller->mark_completed();
      state.restore_pause_controller.reset();
      MessageBoxW(
          state.window,
          L"縮小復元のレビュー済み配置または作業場所の証跡が失われました。対象ディスクは開いていません。",
          L"縮小復元を開始できません",
          MB_OK | MB_ICONERROR);
      return;
    }
    shrink_request =
        ytec::windowsapp::WindowsOnlineShrinkRestoreRequest{
            .reviewed_image = image,
            .reviewed_target = target,
            .image = verify_request,
            .image_backing_disk = protected_log_disks.front(),
            .expected_target = target_identity,
            .expected_target_layout_hash = target_layout_hash,
            .reviewed_layout = std::move(shrink_layout.value()),
            .work_paths = std::move(shrink_work_paths.value()),
            .observed_work = std::move(shrink_work_observation.value()),
            .confirmation = {
                .first_step_acknowledged = true,
                .typed_token = L"OK",
            },
            .administrator = state.elevated,
            .operation_id = restore_operation_id,
            .callbacks = restore_callbacks,
        };
  } else {
    exact_request = ytec::windowsapp::OnlineImageRestoreOperationRequest{
        .reviewed_image = image,
        .reviewed_target = target,
        .restore = {
            .image = verify_request,
            .expected_image_global_hash = image.global_hash,
            .expected_source_state_hash = image.manifest.source_state_hash,
            .expected_target = target_identity,
            .expected_target_layout_hash = target_layout_hash,
            .individual_partition = individual_partition,
            .confirmation = {
                .first_step_acknowledged = true,
                .typed_token = L"OK",
            },
            .administrator = state.elevated,
            .callbacks = restore_callbacks,
        },
        .operation_id = restore_operation_id,
    };
  }
  if (state.logger.has_value()) {
    state.logger->info(
        L"Windows直接.tsumugi復元開始 target_disk=" +
        std::to_wstring(target.disk_number));
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const std::uint64_t completion_power_operation_binding =
      ytec::windowsapp::take_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  state.restore_thread = std::thread(
      [window,
       exact_request = std::move(exact_request),
       shrink_request = std::move(shrink_request),
       restore_password = std::move(restore_password),
       pause_controller,
       completion_power_operation_binding,
       target_disk_number = target.disk_number,
       individual_restore = individual_partition.has_value()]() {
        static_cast<void>(restore_password);
        auto payload = std::make_unique<RestorePayload>();
        payload->target_disk_number = target_disk_number;
        payload->individual_partition = individual_restore;
        payload->completion_power_operation_binding =
            completion_power_operation_binding;
        ThreadSleepPrevention sleep_prevention;
        if (!sleep_prevention.active()) {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"復元中の自動スリープ防止",
              .message = L"自動スリープを安全に防止できないため開始しません",
          };
        } else if (shrink_request.has_value()) {
          auto result = ytec::windowsapp::
              execute_windows_online_shrink_restore_operation_with_windows_apis(
                  shrink_request.value());
          if (result) {
            payload->shrink_report = result.take_value();
          } else {
            payload->error = result.error();
          }
        } else if (exact_request.has_value()) {
          auto result = ytec::windowsapp::
              execute_online_image_restore_operation_with_windows_apis(
                  exact_request.value());
          if (result) {
            payload->report = result.take_value();
          } else {
            payload->error = result.error();
          }
        } else {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::invalid_data,
              .native_code = ERROR_INVALID_DATA,
              .operation = L"復元要求",
            .message = L"通常／縮小復元要求のどちらもありません",
          };
        }
        pause_controller->mark_completed();
        payload->sleep_prevention_release = sleep_prevention.release();
        if (PostMessageW(
                window,
                kRestoreCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void create_online_backup_flow(AppState& state) {
  if (state.backup_running.load()) {
    return;
  }
  const bool rescue_mode = selected_image_rescue_mode(state);
  const auto verification_mode =
      selected_image_create_verification_mode(state);
  if (!verification_mode.has_value()) {
    MessageBoxW(
        state.window,
        L"作成後の検証方式を確認できません。完全または高速を選び直してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  if (!state.elevated) {
    MessageBoxW(
        state.window,
        L"オンラインイメージ作成には管理者権限が必要です。"
        L"\nこの画面からUAC昇格は自動実行しません。"
        L"\n管理者実行の確認は後ほど一緒に行います。",
        kWindowTitle,
        MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto source_index = combo_selection(state.source_combo);
  if (!state.inventory.has_value() || !source_index.has_value() ||
      source_index.value() >= state.inventory->disks.size()) {
    MessageBoxW(
        state.window,
        L"バックアップするディスクを選択してください。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  const auto& source = state.inventory->disks[source_index.value()];
  if (source.partition_style != ytec::diskmodel::PartitionStyle::gpt &&
      source.partition_style != ytec::diskmodel::PartitionStyle::mbr) {
    MessageBoxW(
        state.window,
        L"GPTまたはMBRの基本ディスクだけをバックアップできます。"
        L"不明な形式や動的ディスクは安全のため処理しません。",
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return;
  }
  if (rescue_mode &&
      !current_windows_data_rescue_image_selection_ready(state)) {
    MessageBoxW(
        state.window,
        current_windows_data_rescue_image_selection_message(state).c_str(),
        L"救出イメージ元を確認してください",
        MB_OK | MB_ICONWARNING);
    return;
  }
  if (!confirm_source_health_advice(
          state, source, L"イメージ作成")) {
    return;
  }

  const auto transfer_mode = selected_transfer_mode(state);
  const bool shrink_mode =
      !rescue_mode &&
      transfer_mode == ytec::imageformat::TransferMode::shrink;
  auto source_identity = ytec::diskmodel::make_stable_disk_identity(
      source, source.is_system_disk);
  if (!source_identity) {
    show_product_error(
        state.window,
        L"バックアップ元を安定識別できません",
        source_identity.error());
    return;
  }
  const std::array<ytec::clonecore::StableDiskIdentity, 1U>
      protected_log_disks{source_identity.value()};
  if (!prepare_source_safe_product_logging(
          state, protected_log_disks, L"イメージ作成") ||
      !require_startup_write_access(state, L"イメージ作成")) {
    return;
  }
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const std::wstring_view default_name = rescue_mode
      ? L"Tsumugi-data-rescue.tsumugi"
      : source.is_system_disk ? L"Tsumugi-system-backup.tsumugi"
                              : L"Tsumugi-data-backup.tsumugi";
  std::copy(default_name.begin(), default_name.end(), path.begin());
  constexpr wchar_t kFilter[] =
      L"Tsumugi Drive イメージ (*.tsumugi)\0*.tsumugi\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(OPENFILENAMEW);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter = kFilter;
  dialog.nFilterIndex = 1;
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"tsumugi";
  dialog.lpstrTitle = rescue_mode
      ? L"新しい救出イメージの保存先"
      : L"新しいバックアップイメージの保存先";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                 OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    const DWORD dialog_error = CommDlgExtendedError();
    if (dialog_error != 0) {
      const std::wstring message =
          L"保存先選択画面でエラーが発生しました。\nCommon dialog error: " +
          std::to_wstring(dialog_error);
      MessageBoxW(
          state.window,
          message.c_str(),
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }
  const auto portable_data_gate = ytec::windowsapp::
      require_windows_tsumugi_destination_outside_portable_data(
          path.data());
  if (!portable_data_gate) {
    log_error_summary(
        state.logger,
        L".tsumugi保存先をEXE隣data境界で停止",
        portable_data_gate.error());
    show_product_error(
        state.window,
        L"この場所へイメージを保存できません",
        portable_data_gate.error(),
        L"EXEと同じフォルダーのdata、その配下、AppDataは保存先に使用しません。別のローカルフォルダーを選択してください。");
    return;
  }
  const int encryption_choice = MessageBoxW(
      state.window,
      L"この.tsumugiイメージをパスワードで暗号化しますか？\n\n暗号化にはArgon2idとAES-256-GCMを使用します。回復キーはなく、パスワードを紛失すると復元できません。",
      L"イメージ暗号化",
      MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
  if (encryption_choice == IDCANCEL) {
    return;
  }
  std::shared_ptr<SecureAsciiPassword> backup_password;
  if (encryption_choice == IDYES) {
    auto password_prompt = prompt_tsumugi_password(
        state.window,
        state.body_font,
        L"新しいイメージの暗号化パスワードを2回入力してください。パスワードはイメージ作成の完了または停止時にメモリから消去します。",
        L"イメージを暗号化",
        true,
        false,
        L"このパスワードを使用");
    if (!password_prompt.accepted || password_prompt.password == nullptr) {
      return;
    }
    backup_password = std::move(password_prompt.password);
  }
  if (!confirm_long_operation_power(state, L"イメージ作成")) {
    return;
  }

  const auto pending_restart =
      ytec::windowsapp::query_windows_update_pending_restart();
  if (state.logger.has_value() &&
      pending_restart.state !=
          ytec::windowsapp::PendingRestartState::absent) {
    state.logger->warning(
        pending_restart.state ==
                ytec::windowsapp::PendingRestartState::required
            ? L"Windows Updateの再起動保留を検出"
            : L"Windows Updateの再起動保留状態は不明 native_status=" +
                  std::to_wstring(pending_restart.native_status));
  }
  std::wstring confirmation =
      std::wstring(
          rescue_mode
              ? L"次の既保護データディスクから救出イメージを作成します。\n\n"
              : L"次の読み取り専用コピー元からオンラインイメージを作成します。\n\n") +
      L"ディスク " + std::to_wstring(source.disk_number) + L" / " +
      (source.model.empty() ? L"モデル不明" : source.model) +
      L"\n容量: " + format_bytes(source.size_bytes) +
       L"\n形式: " + partition_style_text(source.partition_style) +
       L"\nモード: " +
        (rescue_mode ? L"救出モード（非systemデータディスク）"
                     : shrink_mode ? L"縮小移行モード" : L"通常モード") +
       L"\n作成後の検証: " +
        std::wstring(image_create_verification_mode_label(
            verification_mode.value())) +
       (backup_password != nullptr
            ? L"\n暗号化: あり（回復キーなし）"
            : L"\n暗号化: なし") +
       L"\n\n保存先:\n" + std::wstring(path.data()) +
      L"\n\nコピー元ディスクへの書き込みは行いません。"
      L"\n既存の保存先は上書きしません。";
  confirmation += rescue_mode
      ? L"\nWindows版はSource属性を変更せず、既にread-onlyまたはofflineのディスクだけを扱います。"
        L"\nSource全体を別物理ディスクの所有一時領域へ1回だけ救出し、全書込み読戻しとflush後にread-only封印します。"
        L"\n単一.tsumugiは封印済み一時領域だけから作成し、選択済み検証、一時領域破棄、保存先再識別後だけ完成名を確定します。"
        L"\n欠損は有限再試行後にゼロ埋めmapへ記録し、欠損ゼロでも通常成功へ読み替えません。"
      : shrink_mode
      ? L"\nNTFS領域は同一VSS Snapshot Setから単一WIMへ取り込み、静的システム領域は固定した読取り専用ハンドルからexact RAWで取得します。"
        L"\nBitLocker、exFAT、FAT32の内容領域は現在のWindows直接縮小作成では開始前に拒否します。"
      : L"\nNTFS領域は同一VSS Snapshot Setから読み、静的システム領域は二重Hashで変化を検出します。";
  confirmation += verification_mode.value() ==
          ytec::imageformat::TsumugiCreateVerificationMode::complete
      ? L"\n完全検証では、各書込み読戻し、認証・Hash、最終メタデータ検証に加え、完成前の追加全走査も実行します。"
      : L"\n高速検証では、各書込み読戻し、認証・Hash、最終メタデータ検証を維持し、完成前の追加全走査だけを省略します。";
  confirmation += ytec::windowsapp::pending_restart_confirmation_note(
      pending_restart.state);
  confirmation += L"\n\n開始しますか？";
  if (MessageBoxW(
          state.window,
          confirmation.c_str(),
          rescue_mode ? L"救出イメージの確認"
                      : L"オンラインイメージの確認",
          MB_YESNO |
              (pending_restart.state ==
                       ytec::windowsapp::PendingRestartState::absent
                   ? MB_ICONINFORMATION
                   : MB_ICONWARNING) |
              MB_DEFBUTTON2) != IDYES) {
    return;
  }

  const auto version = current_windows_version();
  const std::string architecture = current_native_architecture();
  if (!version || architecture.empty()) {
    if (!version) {
      show_product_error(
          state.window,
          L"Windowsの環境を確認できません",
          version.error());
    } else {
      MessageBoxW(
          state.window,
          L"AMD64版Windowsとして安全に識別できませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return;
  }

  state.backup_cancel_requested.store(false);
  state.backup_progress.reset();
  state.backup_elapsed = std::chrono::milliseconds::zero();
  state.backup_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.backup_pause_controller;
  state.backup_running.store(true);
  const ULONGLONG backup_started_tick = GetTickCount64();
  auto last_progress_tick =
      std::make_shared<std::atomic<ULONGLONG>>(0);
  auto last_progress_stage =
      std::make_shared<std::atomic<std::uint8_t>>(
          static_cast<std::uint8_t>(0xFFU));
  if (state.logger.has_value()) {
    state.logger->info(
        L"オンラインイメージ作成開始 source_disk=" +
        std::to_wstring(source.disk_number) + L" source_bytes=" +
        std::to_wstring(source.size_bytes) + L" mode=" +
        (rescue_mode ? L"rescue" : shrink_mode ? L"shrink" : L"exact") +
        L" verification=" +
        (verification_mode.value() ==
                 ytec::imageformat::TsumugiCreateVerificationMode::complete
             ? L"complete"
             : L"fast"));
  }
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND window = state.window;
  const auto async_wait = ytec::vssrequester::AsyncWaitOptions{
      .timeout_ms = 120'000,
      .poll_interval_ms = 250,
      .cancellation_requested =
          [&state]() {
            return state.backup_cancel_requested.load();
          },
  };
  const auto callbacks = ytec::clonecore::bind_manual_pause_controller(
      ytec::clonecore::DiskOperationCallbacks{
      .progress =
          [window,
           backup_started_tick,
           last_progress_tick,
           last_progress_stage](
              const ytec::clonecore::DiskOperationProgress& progress) {
            const ULONGLONG now = GetTickCount64();
            const auto stage = static_cast<std::uint8_t>(progress.stage);
            const bool stage_changed =
                last_progress_stage->exchange(stage) != stage;
            const ULONGLONG previous = last_progress_tick->load();
            if (!stage_changed &&
                progress.stage !=
                    ytec::clonecore::DiskOperationStage::completed &&
                now - previous < 200U) {
              return;
            }
            last_progress_tick->store(now);
            auto update = std::make_unique<BackupProgressPayload>();
            update->progress = progress;
            update->elapsed =
                std::chrono::milliseconds(now - backup_started_tick);
            if (PostMessageW(
                    window,
                    kBackupProgressMessage,
                    0,
                    reinterpret_cast<LPARAM>(update.get())) != FALSE) {
              static_cast<void>(update.release());
            }
          },
      .cancellation_requested =
          [&state]() {
            return state.backup_cancel_requested.load();
          },
      },
      pause_controller);
  const auto* logger =
      state.logger.has_value() ? &state.logger.value() : nullptr;
  const std::string created_utc = current_utc_timestamp();
  const auto encryption_password = backup_password != nullptr
      ? std::optional<std::string_view>(backup_password->view())
      : std::nullopt;
  auto exact_request = ytec::windowsapp::OnlineImageCreateRequest{
      .selected_source = source,
      .final_path = path.data(),
      .administrator = state.elevated,
      .windows_major = version.value()[0],
      .windows_minor = version.value()[1],
      .windows_build = version.value()[2],
      .windows_architecture = architecture,
      .created_utc = created_utc,
      .app_version = std::string(kAppVersion),
      .encryption_password = encryption_password,
      .verification_mode = verification_mode.value(),
      .replace_existing = false,
      .async_wait = async_wait,
      .callbacks = callbacks,
      .logger = logger,
  };
  auto shrink_request =
      ytec::windowsapp::WindowsOnlineShrinkImageProductRequest{
          .selected_source = source,
          .final_path = path.data(),
          .administrator = state.elevated,
          .windows_major = version.value()[0],
          .windows_minor = version.value()[1],
          .windows_build = version.value()[2],
          .windows_architecture = architecture,
          .created_utc = created_utc,
          .app_version = std::string(kAppVersion),
          .encryption_password = encryption_password,
          .verification_mode = verification_mode.value(),
          .replace_existing = false,
          .async_wait = async_wait,
          .callbacks = callbacks,
          .logger = logger,
          .persistent_log_path = state.log_path,
          .log_is_ram_only = state.log_path.empty(),
  };
  const std::uint64_t completion_power_operation_binding =
      ytec::windowsapp::take_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  state.backup_thread = std::thread(
      [window,
       shrink_mode,
       rescue_mode,
       exact_request = std::move(exact_request),
       shrink_request = std::move(shrink_request),
       backup_password = std::move(backup_password),
       pause_controller,
       completion_power_operation_binding]() {
        static_cast<void>(backup_password);
        auto payload = std::make_unique<BackupPayload>();
        payload->final_path = exact_request.final_path;
        payload->rescue_mode = rescue_mode;
        payload->completion_power_operation_binding =
            completion_power_operation_binding;
        ThreadSleepPrevention sleep_prevention;
        if (!sleep_prevention.active()) {
          payload->error = ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_NOT_SUPPORTED,
              .operation = L"イメージ作成中の自動スリープ防止",
              .message = L"自動スリープを安全に防止できないため開始しません",
          };
        } else {
          if (rescue_mode) {
            auto result = ytec::windowsapp::
                execute_windows_data_rescue_image_create_with_windows_apis(
                    exact_request);
            if (result) {
              payload->rescue_report = result.take_value();
            } else {
              payload->error = result.error();
            }
          } else {
            auto result = shrink_mode
                ? ytec::windowsapp::
                      execute_windows_online_shrink_image_create_with_windows_apis(
                          shrink_request)
                : ytec::windowsapp::
                      execute_online_image_create_with_windows_apis(
                          exact_request);
            if (result) {
              payload->report = result.take_value();
            } else {
              payload->error = result.error();
            }
          }
        }
        pause_controller->mark_completed();
        payload->sleep_prevention_release = sleep_prevention.release();
        if (PostMessageW(
                window,
                kBackupCompleteMessage,
                0,
                reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
          static_cast<void>(payload.release());
        }
      });
}

void update_action_state(AppState& state) {
  const bool source_page =
      state.page == Page::clone || state.page == Page::create_image;
  const bool clone_page = state.page == Page::clone;
  const bool restore_target_page =
      state.page == Page::restore_image &&
      state.restore_preflight.has_value() &&
      !state.restore_preflight_running.load();
  const bool rescue_page = state.page == Page::rescue_media;
  const bool rescue_options_visible =
      rescue_page && state.media_preflight.has_value() &&
      !state.adk_management_running.load() &&
      !state.media_creation_running.load() &&
      !state.media_creation_report.has_value();
  const bool media_environment_ready =
      rescue_page && state.media_preflight.has_value() &&
      state.media_preflight->media_creation_permitted &&
      !state.media_preflight_running.load();
  const bool media_iso_page =
      rescue_options_visible &&
      selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::iso_file;
  const bool media_usb_page =
      rescue_options_visible &&
      selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::usb_drive;
  RECT client{};
  GetClientRect(state.window, &client);
  const auto clone_layout =
      ytec::windowsapp::calculate_clone_column_layout(client.right);
  if (state.page == Page::create_image) {
    MoveWindow(
        state.source_combo,
        312,
        288,
        (std::max)(client.right - 374, 560L),
        280,
        TRUE);
  } else {
    MoveWindow(
        state.source_combo,
        clone_layout.source_control.left,
        236,
        clone_layout.source_control.width(),
        280,
        TRUE);
  }
  if (restore_target_page) {
    MoveWindow(
        state.target_combo,
        312,
        client.bottom - 218,
        (std::max)(client.right - 374, 560L),
        280,
        TRUE);
  } else if (media_usb_page) {
    const auto vertical = ytec::windowsapp::
        calculate_rescue_media_vertical_layout(client.bottom, true);
    MoveWindow(
        state.target_combo,
        312,
        vertical.destination_control_top,
        (std::max)(client.right - 374, 560L),
        280,
        TRUE);
  } else {
    MoveWindow(
        state.target_combo,
        clone_layout.target_control.left,
        236,
        clone_layout.target_control.width(),
        280,
        TRUE);
  }
  MoveWindow(
      state.restore_source_partition_combo,
      312,
      client.bottom - 282,
      (std::max)(client.right - 374, 560L),
      280,
      TRUE);
  MoveWindow(
      state.restore_target_partition_combo,
      312,
      client.bottom - 154,
      (std::max)(client.right - 374, 560L),
      280,
      TRUE);
  ShowWindow(state.source_combo, source_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.transfer_mode_combo,
      source_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.image_verification_mode_combo,
      state.page == Page::create_image ? SW_SHOW : SW_HIDE);
  EnableWindow(
      state.transfer_mode_combo,
      source_page && !state.backup_running.load() &&
              !state.clone_running.load() &&
              !state.inventory_loading.load()
          ? TRUE
          : FALSE);
  EnableWindow(
      state.image_verification_mode_combo,
      state.page == Page::create_image &&
              !state.backup_running.load() &&
              !state.inventory_loading.load()
          ? TRUE
          : FALSE);
  EnableWindow(
      state.source_combo,
      source_page && !state.inventory_loading.load() &&
              !state.backup_running.load() &&
              !state.clone_running.load()
          ? TRUE
          : FALSE);
  ShowWindow(
      state.target_combo,
      clone_page || restore_target_page || media_usb_page
          ? SW_SHOW
          : SW_HIDE);
  ShowWindow(
      state.restore_source_partition_combo,
      restore_target_page ? SW_SHOW : SW_HIDE);
  const bool individual_restore = restore_target_page &&
      selected_restore_source_partition(state).has_value();
  ShowWindow(
      state.restore_target_partition_combo,
      individual_restore ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.restore_change_image,
      restore_target_page && !state.restore_running.load()
          ? SW_SHOW
          : SW_HIDE);
  const auto media_layout =
      ytec::windowsapp::calculate_rescue_media_control_layout(
          client.right);
  const auto media_vertical = ytec::windowsapp::
      calculate_rescue_media_vertical_layout(
          client.bottom, media_usb_page);
  MoveWindow(
      state.media_kind_combo,
      media_layout.kind_control.left,
      media_vertical.kind_control_top,
      media_layout.kind_control.width(),
      180,
      TRUE);
  MoveWindow(
      state.media_profile_combo,
      media_layout.profile_control.left,
      media_vertical.kind_control_top,
      media_layout.profile_control.width(),
      180,
      TRUE);
  MoveWindow(
      state.media_usb_mode_combo,
      media_layout.mode_control.left,
      media_vertical.option_control_top,
      media_layout.mode_control.width(),
      180,
      TRUE);
  MoveWindow(
      state.media_usb_file_system_combo,
      media_layout.file_system_control.left,
      media_vertical.option_control_top,
      media_layout.file_system_control.width(),
      180,
      TRUE);
  MoveWindow(
      state.media_output_edit,
      media_layout.output_edit.left,
      media_vertical.destination_control_top,
      media_layout.output_edit.width(),
      34,
      TRUE);
  MoveWindow(
      state.media_browse,
      media_layout.browse_button.left,
      media_vertical.destination_control_top,
      media_layout.browse_button.width(),
      34,
      TRUE);
  ShowWindow(
      state.media_kind_combo,
      rescue_options_visible ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_profile_combo,
      rescue_options_visible ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_usb_mode_combo,
      media_usb_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_usb_file_system_combo,
      media_usb_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_output_edit,
      media_iso_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.media_browse,
      media_iso_page ? SW_SHOW : SW_HIDE);
  const bool media_controls_enabled =
      media_environment_ready && !state.backup_running.load() &&
      !state.adk_management_running.load() &&
      !state.media_creation_running.load() &&
      !state.media_usb_inspection_running.load();
  const auto media_usb_ui = current_rescue_media_usb_ui_view(state);
  EnableWindow(
      state.media_kind_combo,
      media_controls_enabled ? TRUE : FALSE);
  EnableWindow(
      state.media_profile_combo,
      media_controls_enabled ? TRUE : FALSE);
  EnableWindow(
      state.media_usb_mode_combo,
      media_controls_enabled && media_usb_page &&
              media_usb_ui.mode_selector_enabled
          ? TRUE
          : FALSE);
  EnableWindow(
      state.media_usb_file_system_combo,
      media_controls_enabled && media_usb_page &&
              media_usb_ui.file_system_selector_enabled
          ? TRUE
          : FALSE);
  EnableWindow(
      state.media_output_edit,
      media_controls_enabled && media_iso_page ? TRUE : FALSE);
  EnableWindow(
      state.media_browse,
      media_controls_enabled && media_iso_page ? TRUE : FALSE);
  if (media_usb_page) {
    EnableWindow(
        state.target_combo,
        media_controls_enabled && !state.inventory_loading.load()
            ? TRUE
            : FALSE);
  } else {
    EnableWindow(
        state.target_combo,
        (clone_page && state.clone_running.load()) ||
                (state.page == Page::restore_image &&
                 state.restore_running.load())
            ? FALSE
            : TRUE);
  }
  EnableWindow(
      state.restore_source_partition_combo,
      restore_target_page && !state.restore_running.load() ? TRUE : FALSE);
  EnableWindow(
      state.restore_target_partition_combo,
      individual_restore && !state.restore_running.load() ? TRUE : FALSE);

  std::wstring action = L"安全確認へ";
  bool enabled = false;
  if (state.page == Page::clone) {
    action = state.clone_running.load()
        ? state.clone_cancel_requested.load()
              ? L"安全な停止を待っています…"
              : L"安全に取り消す"
        : L"安全確認へ";
    enabled = state.clone_running.load()
        ? !state.clone_cancel_requested.load() &&
              (!state.clone_progress.has_value() ||
               state.clone_progress->cancellation_allowed)
        : (selected_clone_rescue_mode(state)
               ? current_windows_data_rescue_selection_ready(state)
               : current_clone_selection(state).ready) &&
              state.elevated;
  } else if (state.page == Page::create_image) {
    action = state.backup_running.load()
        ? state.backup_cancel_requested.load()
              ? L"安全な停止を待っています…"
              : L"安全に取り消す"
        : L"保存先を選ぶ";
    const auto source_index = combo_selection(state.source_combo);
    enabled = state.backup_running.load()
        ? !state.backup_cancel_requested.load() &&
              (!state.backup_progress.has_value() ||
               state.backup_progress->cancellation_allowed)
        : state.elevated &&
              selected_image_create_verification_mode(state).has_value() &&
              (selected_image_rescue_mode(state)
                   ? current_windows_data_rescue_image_selection_ready(state)
                   : !state.inventory_loading.load() &&
                         state.inventory.has_value() &&
                         source_index.has_value() &&
                         source_index.value() <
                             state.inventory->disks.size() &&
                         (state.inventory->disks[source_index.value()]
                                  .partition_style ==
                              ytec::diskmodel::PartitionStyle::gpt ||
                          state.inventory->disks[source_index.value()]
                                  .partition_style ==
                              ytec::diskmodel::PartitionStyle::mbr));
  } else if (state.page == Page::restore_image) {
    action = state.restore_running.load()
                 ? state.restore_cancel_requested.load()
                       ? L"安全な停止を待っています…"
                       : L"安全に取り消す"
                 : state.restore_preflight_running.load()
                       ? L"イメージを検証中…"
                       : state.restore_preflight.has_value()
                             ? L"直接復元の安全確認へ"
                             : L"イメージを選ぶ";
    enabled = state.restore_running.load()
        ? !state.restore_cancel_requested.load() &&
              (!state.restore_progress.has_value() ||
               state.restore_progress->cancellation_allowed)
        : !state.restore_preflight_running.load() &&
              !state.backup_running.load() &&
              !state.media_preflight_running.load() &&
              (!state.restore_preflight.has_value() ||
               current_restore_target_selection(state)
                   .ready_for_confirmation);
    EnableWindow(
        state.restore_change_image,
        !state.restore_preflight_running.load() &&
                !state.restore_running.load()
            ? TRUE
            : FALSE);
  } else if (state.page == Page::boot_repair) {
    action = L"レスキューメディアを作る";
    enabled = !state.backup_running.load() &&
              !state.media_creation_running.load();
  } else if (state.page == Page::rescue_media) {
    if (state.adk_management_running.load()) {
      action = L"ADK安全ゲートを確認中…";
      enabled = false;
    } else if (state.media_creation_running.load()) {
      const bool can_cancel =
          state.media_creation_progress.has_value() &&
          state.media_creation_progress->cancellation_allowed &&
          !state.media_creation_cancel_requested.load();
      action = state.media_creation_cancel_requested.load()
                   ? L"安全な停止を待っています…"
                   : can_cancel
                         ? L"安全に取り消す"
                         : selected_media_kind(state) ==
                                   ytec::windowsapp::RescueMediaKind::usb_drive
                               ? L"USBを作成中…"
                               : L"ISOを作成中…";
      enabled = can_cancel;
    } else if (state.media_creation_report.has_value()) {
      action =
          state.media_creation_report->complete_usb_verified
              ? L"別のUSBを作成"
              : L"別のISOを作成";
      enabled = true;
    } else if (state.media_preflight_running.load()) {
      action = L"ADK／WinPEを確認中…";
    } else if (!state.media_preflight.has_value() ||
               !state.media_preflight->media_creation_permitted) {
      action = state.media_preflight.has_value()
                   ? L"ADK導入ガイド／再確認"
                   : L"作成環境を確認";
      enabled = !state.backup_running.load();
    } else if (state.media_usb_inspection_running.load()) {
      action = L"USBの所有情報を検査中…";
      enabled = false;
    } else {
      const auto plan = current_rescue_media_plan(state);
      if (plan.issue ==
              ytec::windowsapp::RescueMediaPlanIssue::
                  iso_destination_missing ||
          plan.issue ==
              ytec::windowsapp::RescueMediaPlanIssue::
                  iso_destination_invalid) {
        action = L"ISOの保存先を選ぶ";
        enabled = !state.backup_running.load();
      } else if (plan.ready_for_confirmation) {
        action =
            selected_media_kind(state) ==
                    ytec::windowsapp::RescueMediaKind::usb_drive
                ? state.elevated
                      ? L"USB作成内容を確認"
                      : L"USB内容と管理者要件を確認"
                : state.elevated
                      ? L"作成内容を確認"
                      : L"作成内容と管理者要件を確認";
        enabled = !state.backup_running.load();
      } else {
        action = L"選択内容を確認してください";
      }
    }
  } else {
    action = state.support_zip_planning.load()
        ? L"含有一覧を準備中…"
        : state.support_zip_creation_running.load()
              ? L"サポートZIPを作成中…"
              : std::wstring(
                    ytec::windowsapp::support_zip_ui_contract().action_label);
    enabled = !state.support_zip_planning.load() &&
        !state.support_zip_creation_running.load() &&
        !state.clone_running.load() && !state.backup_running.load() &&
        !state.restore_running.load() &&
        !state.media_creation_running.load() &&
        !state.inventory_loading.load() &&
        !state.manual_update_running.load() &&
        !state.adk_management_running.load() &&
        !state.media_preflight_running.load() &&
        !state.media_usb_inspection_running.load() &&
        !state.restore_preflight_running.load() &&
        state.startup_data_policy.write_operations_permitted();
  }
  const bool write_entry_selected =
      (state.page == Page::clone && !state.clone_running.load()) ||
      (state.page == Page::create_image && !state.backup_running.load()) ||
      (state.page == Page::restore_image &&
       state.restore_preflight.has_value() &&
       !state.restore_running.load()) ||
      (state.page == Page::rescue_media &&
       !state.media_creation_running.load() &&
       !state.media_creation_report.has_value() &&
       current_rescue_media_plan(state).ready_for_confirmation);
  if (write_entry_selected &&
      state.startup_data_policy.diagnostic_only()) {
    action = L"診断専用（dataを書き込めません）";
    enabled = false;
  }
  if (state.adk_management_running.load()) {
    action = L"ADK取得・管理を確認中…";
    enabled = false;
  }
  SetWindowTextW(state.primary_action, action.c_str());
  EnableWindow(state.primary_action, enabled ? TRUE : FALSE);

  bool pause_operation_running = false;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      pause_controller;
  if (state.page == Page::clone && state.clone_running.load()) {
    pause_operation_running = true;
    pause_controller = state.clone_pause_controller;
  } else if (
      state.page == Page::create_image && state.backup_running.load()) {
    pause_operation_running = true;
    pause_controller = state.backup_pause_controller;
  } else if (
      state.page == Page::restore_image && state.restore_running.load()) {
    pause_operation_running = true;
    pause_controller = state.restore_pause_controller;
  }
  update_manual_pause_button(
      state.pause_action, pause_operation_running, pause_controller);
  const bool diagnostics_page = state.page == Page::diagnostics;
  ShowWindow(
      state.manual_update_action,
      diagnostics_page ? SW_SHOW : SW_HIDE);
  ShowWindow(
      state.first_run_guidance_action,
      diagnostics_page ? SW_SHOW : SW_HIDE);
  SetWindowTextW(
      state.manual_update_action,
      state.manual_update_running.load()
          ? L"更新を確認中…"
          : L"更新を確認");
  EnableWindow(
      state.manual_update_action,
      diagnostics_page && !state.manual_update_running.load() &&
              !state.adk_management_running.load() &&
              !state.support_zip_planning.load() &&
              !state.support_zip_creation_running.load()
          ? TRUE
          : FALSE);
  SetWindowTextW(
      state.first_run_guidance_action,
      L"安全ガイド");
  EnableWindow(
      state.first_run_guidance_action,
      diagnostics_page && !state.support_zip_planning.load() &&
              !state.adk_management_running.load() &&
              !state.support_zip_creation_running.load()
          ? TRUE
          : FALSE);
  EnableWindow(
      state.refresh,
      !state.inventory_loading.load() &&
              !state.adk_management_running.load() &&
              !state.media_usb_inspection_running.load() &&
              !state.support_zip_planning.load() &&
              !state.support_zip_creation_running.load()
          ? TRUE
          : FALSE);
}

void populate_disk_combos(AppState& state) {
  SendMessageW(state.source_combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.target_combo, CB_RESETCONTENT, 0, 0);
  if (!state.inventory.has_value()) {
    update_action_state(state);
    return;
  }
  for (const auto& disk : state.inventory->disks) {
    const std::wstring label = disk_label(disk);
    SendMessageW(
        state.source_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(
        state.target_combo,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
  }
  std::optional<std::size_t> source_index;
  for (std::size_t index = 0; index < state.inventory->disks.size(); ++index) {
    if (state.inventory->disks[index].is_system_disk) {
      source_index = index;
      break;
    }
  }
  if (!source_index.has_value() && !state.inventory->disks.empty()) {
    source_index = 0;
  }
  if (source_index.has_value()) {
    SendMessageW(
        state.source_combo,
        CB_SETCURSEL,
        static_cast<WPARAM>(source_index.value()),
        0);
    const auto& source = state.inventory->disks[source_index.value()];
    for (std::size_t index = 0;
         index < state.inventory->disks.size();
         ++index) {
      const auto& candidate = state.inventory->disks[index];
      if (index != source_index.value() && !candidate.is_system_disk &&
          candidate.read_only == false &&
          candidate.size_bytes >= source.size_bytes) {
        SendMessageW(
            state.target_combo,
            CB_SETCURSEL,
            static_cast<WPARAM>(index),
            0);
        break;
      }
    }
  }
  if (state.page == Page::restore_image &&
      state.restore_preflight.has_value()) {
    populate_restore_source_partition_candidates(state);
    select_default_restore_target(state);
  } else if (
      state.page == Page::rescue_media &&
      selected_media_kind(state) ==
          ytec::windowsapp::RescueMediaKind::usb_drive) {
    select_default_media_usb_target(state);
  }
  update_action_state(state);
}

void start_inventory(AppState& state) {
#if defined(YTEC_UI_ACCEPTANCE_BUILD)
  state.inventory.reset();
  state.inventory_error =
      L"UI受入モード: 物理ディスク列挙と製品I/Oを無効化しています。";
  state.inventory_loading.store(false);
  EnableWindow(state.refresh, FALSE);
  populate_disk_combos(state);
  InvalidateRect(state.window, nullptr, TRUE);
  return;
#else
  if (state.clone_running.load() || state.restore_running.load()) {
    return;
  }
  if (state.inventory_loading.exchange(true)) {
    return;
  }
  if (state.inventory_thread.joinable()) {
    state.inventory_thread.join();
  }
  state.inventory_error.clear();
  EnableWindow(state.refresh, FALSE);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  const auto logger = state.logger;
  if (logger.has_value()) {
    logger->info(L"読み取り専用ディスク列挙開始");
  }
  state.inventory_thread = std::thread([window, logger]() {
    auto payload = std::make_unique<InventoryPayload>();
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider(
            logger.has_value() ? &logger.value() : nullptr);
    const auto result = provider->enumerate();
    if (result) {
      payload->report = result.value();
      log_inventory_summary(logger, result.value());
    } else {
      payload->error =
          L"ディスク情報を取得できませんでした: " +
          result.error().message;
      log_error_summary(
          logger, L"読み取り専用ディスク列挙失敗", result.error());
    }
    if (PostMessageW(
            window,
            kInventoryCompleteMessage,
            0,
            reinterpret_cast<LPARAM>(payload.get())) != FALSE) {
      static_cast<void>(payload.release());
    }
  });
#endif
}

void paint_sidebar(const AppState& state, HDC dc, const RECT& client) {
  RECT sidebar{0, 0, 250, client.bottom};
  const HBRUSH brush = CreateSolidBrush(kSidebar);
  FillRect(dc, &sidebar, brush);
  DeleteObject(brush);

  RECT brand{24, 28, 226, 64};
  draw_text(
      dc,
      L"Y-TEC",
      brand,
      RGB(202, 215, 226),
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);
  RECT title{24, 57, 226, 105};
  draw_text(
      dc,
      L"Tsumugi Drive",
      title,
      RGB(255, 255, 255),
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.brand_font);
  draw_strands(dc, 25, 108, 172);
  RECT concept_bounds{25, 133, 226, 153};
  draw_text(
      dc,
      L"3つの工程を、ひとつに",
      concept_bounds,
      RGB(154, 177, 192),
      DT_LEFT | DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS,
      state.small_font);

  RECT mode{24, client.bottom - 69, 226, client.bottom - 25};
  const wchar_t* mode_text = nullptr;
  if (state.startup_data_policy.diagnostic_only()) {
    mode_text = state.elevated
        ? L"管理者権限・診断専用\ndataへ書き込めません"
        : L"標準権限・診断専用\n書き込み操作は行いません";
  } else if (state.startup_data_policy.intentionally_ram_isolated()) {
    mode_text = state.elevated
        ? L"管理者権限・RAMログ\ndataへは書き込みません"
        : L"標準権限・RAMログ\ndataへは書き込みません";
  } else {
    mode_text = state.elevated
        ? L"管理者権限\n実行操作は安全確認後のみ"
        : L"標準権限・診断モード\n書き込み操作は行いません";
  }
  draw_text(
      dc,
      mode_text,
      mode,
      RGB(173, 191, 205),
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_header(const AppState& state, HDC dc, const RECT& client) {
  const std::size_t index = static_cast<std::size_t>(state.page);
  RECT heading{286, 24, client.right - 172, 64};
  draw_text(
      dc,
      kNavigationLabels[index],
      heading,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.heading_font);

  const bool shows_transfer_mode =
      state.page == Page::clone || state.page == Page::create_image;
  const auto image_options =
      ytec::windowsapp::calculate_image_create_option_layout(client.right);
  const int lead_right = state.page == Page::create_image
      ? image_options.verification_control.left - 10
      : shows_transfer_mode ? client.right - 322 : client.right - 36;
  RECT lead{
      286,
      68,
      (std::max)(286, lead_right),
      114};
  std::wstring text;
  switch (state.page) {
    case Page::clone:
      text =
          L"コピー元とコピー先を選びます。";
      break;
    case Page::create_image:
      text = L"";
      break;
    case Page::restore_image:
      text =
          L"イメージの整合性とコピー先を確認してから復元します。";
      break;
    case Page::boot_repair:
      text =
          L"クローンとは別に、Windowsの起動構成だけを診断・修復します。";
      break;
    case Page::rescue_media:
      text =
          L"このPCのADKを使い、BIOS／UEFI対応のUSBまたはISOを作成します。";
      break;
    case Page::diagnostics:
      text =
          L"ディスク列挙結果、権限状態、ログを安全に確認します。";
      break;
  }
  draw_text(
      dc,
      text,
      lead,
      kMuted,
      DT_LEFT | DT_WORDBREAK | DT_VCENTER,
      state.body_font);
}

void paint_stepper(const AppState& state, HDC dc, const RECT& client) {
  const RECT card{286, 118, client.right - 36, 188};
  fill_rounded_rect(dc, card, kCard, kBorder);
  std::array<std::wstring_view, 4> labels{
      L"1  選択", L"2  安全確認", L"3  実行", L"4  完了"};
  int active_step{};
  if (state.page == Page::clone && state.clone_running.load()) {
    active_step = 2;
  }
  if (state.page == Page::restore_image &&
      state.restore_running.load()) {
    active_step = 2;
  }
  if (state.page == Page::rescue_media) {
    labels = {
        L"1  環境確認",
        L"2  形式・出力先",
        L"3  内容確認",
        L"4  作成"};
    const auto plan = current_rescue_media_plan(state);
    active_step =
        (std::min)(
            static_cast<int>(plan.current_step) - 1,
            3);
  }
  const int width = (card.right - card.left - 36) / 4;
  for (int index = 0; index < 4; ++index) {
    RECT label{
        card.left + 18 + index * width,
        card.top + 17,
        card.left + 18 + (index + 1) * width,
        card.bottom - 13};
    draw_text(
        dc,
        labels[static_cast<std::size_t>(index)],
        label,
        index < active_step
            ? kSafeGreen
            : index == active_step ? kTsumugiBlue : kMuted,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.body_font);
    if (index < 3) {
      const HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
      const auto old = SelectObject(dc, pen);
      const int x = label.right - 20;
      MoveToEx(dc, x, card.top + 25, nullptr);
      LineTo(dc, x, card.bottom - 25);
      SelectObject(dc, old);
      DeleteObject(pen);
    }
  }
}

void paint_partition_bar(
    HDC dc,
    const ytec::diskmodel::DiskInfo& disk,
    RECT area) {
  if (disk.size_bytes == 0 || disk.partitions.empty()) {
    fill_rounded_rect(dc, area, RGB(235, 239, 242), kBorder, 8);
    return;
  }
  constexpr std::array<COLORREF, 5> colors{
      RGB(61, 151, 169),
      RGB(121, 91, 174),
      RGB(72, 135, 197),
      RGB(76, 159, 111),
      RGB(196, 137, 56)};
  int left = area.left;
  for (std::size_t index = 0; index < disk.partitions.size(); ++index) {
    const auto& partition = disk.partitions[index];
    const long double ratio =
        static_cast<long double>(partition.size_bytes) /
        static_cast<long double>(disk.size_bytes);
    int width = static_cast<int>(
        ratio * static_cast<long double>(area.right - area.left));
    width = (std::max)(width, 5);
    const int right =
        index + 1 == disk.partitions.size()
        ? area.right
        : (std::min)(left + width, static_cast<int>(area.right));
    RECT segment{left, area.top, right, area.bottom};
    const HBRUSH brush =
        CreateSolidBrush(colors[index % colors.size()]);
    FillRect(dc, &segment, brush);
    DeleteObject(brush);
    left = right;
    if (left >= area.right) {
      break;
    }
  }
  FrameRect(dc, &area, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

void paint_disk_details(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const auto layout =
      ytec::windowsapp::calculate_clone_column_layout(client.right);
  const std::array<int, 2> selections{
      static_cast<int>(SendMessageW(
          state.source_combo, CB_GETCURSEL, 0, 0)),
      static_cast<int>(SendMessageW(
          state.target_combo, CB_GETCURSEL, 0, 0))};
  const std::array<std::wstring_view, 2> roles{
      L"コピー元（読み取りのみ）", L"コピー先（上書き対象）"};
  for (int column = 0; column < 2; ++column) {
    const auto bounds = column == 0
        ? layout.source_card
        : layout.target_card;
    const int left = bounds.left;
    const int width = bounds.width();
    const RECT card{bounds.left, 206, bounds.right, 420};
    fill_rounded_rect(dc, card, kCard, kBorder);
    RECT role{left + 18, 213, left + width - 18, 236};
    draw_text(
        dc,
        roles[static_cast<std::size_t>(column)],
        role,
        column == 0 ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);

    if (state.inventory_loading.load()) {
      RECT loading{left + 18, 277, left + width - 18, 350};
      draw_text(
          dc,
          L"ディスクを読み取り専用で確認しています…",
          loading,
          kMuted,
          DT_CENTER | DT_SINGLELINE | DT_VCENTER,
          state.body_font);
      continue;
    }
    const int selected = selections[static_cast<std::size_t>(column)];
    if (!state.inventory.has_value() || selected < 0 ||
        static_cast<std::size_t>(selected) >=
            state.inventory->disks.size()) {
      RECT empty{left + 18, 277, left + width - 18, 350};
      draw_text(
          dc,
          state.inventory_error.empty()
              ? L"対象のディスクがありません"
              : state.inventory_error,
          empty,
          state.inventory_error.empty() ? kMuted : RGB(176, 56, 56),
          DT_CENTER | DT_WORDBREAK | DT_VCENTER,
          state.body_font);
      continue;
    }
    const auto& disk =
        state.inventory->disks[static_cast<std::size_t>(selected)];
    RECT summary{left + 18, 277, left + width - 18, 310};
    const std::wstring summary_text =
        partition_style_text(disk.partition_style) + L"  •  " +
        (disk.bus_type.empty() ? L"Bus不明" : disk.bus_type) + L"  •  " +
        std::to_wstring(disk.logical_sector_size) + L" / " +
        std::to_wstring(disk.physical_sector_size) + L" bytes";
    draw_text(
        dc,
        summary_text,
        summary,
        kMuted,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    RECT bar{left + 18, 321, left + width - 18, 345};
    paint_partition_bar(dc, disk, bar);
    RECT parts{left + 18, 352, left + width - 18, 382};
    const std::wstring partition_text =
        std::to_wstring(disk.partitions.size()) + L" パーティション  •  " +
        (disk.serial_suffix.empty()
             ? L"シリアル末尾なし"
             : L"シリアル末尾 " +
                   std::wstring(
                       disk.serial_suffix.begin(),
                       disk.serial_suffix.end()));
    draw_text(
        dc,
        partition_text,
        parts,
        kInk,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    RECT badge{left + 18, 383, left + width - 18, 406};
    const std::wstring badge_text =
        (disk.is_system_disk ? L"● 現在のWindows  •  " : L"● ") +
        disk_health_summary(disk);
    const COLORREF health_color =
        disk.health.state == ytec::diskmodel::DiskHealthState::failing
            ? RGB(176, 56, 56)
            : disk.health.state ==
                      ytec::diskmodel::DiskHealthState::caution ||
                  disk.health.temperature_warning
                ? kWarning
                : disk.health.state ==
                          ytec::diskmodel::DiskHealthState::healthy
                      ? kSafeGreen
                      : kMuted;
    draw_text(
        dc,
        badge_text,
        badge,
        health_color,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
  }
}

void paint_progress_preview(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const RECT card{286, 438, client.right - 36, client.bottom - 92};
  fill_rounded_rect(dc, card, kCard, kBorder);
  if (card.bottom - card.top < 112) {
    std::wstring compact_status = L"進行状況  •  ";
    if (state.clone_running.load()) {
      const auto observed = state.clone_progress.value_or(
          ytec::clonecore::DiskOperationProgress{
              .stage = ytec::clonecore::DiskOperationStage::planning,
              .cancellation_allowed = true,
          });
      const auto compact_progress =
          ytec::windowsapp::build_online_image_progress_view(
              observed, state.clone_elapsed);
      compact_status += compact_progress.stage_label + L"  " +
                        compact_progress.percentage_label;
    } else {
      compact_status += L"開始前 — 実行開始前";
    }
    RECT compact_text{
        card.left + 20,
        card.top + 2,
        card.right - 20,
        card.bottom - 2};
    draw_text(
        dc,
        compact_status,
        compact_text,
        kMuted,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    return;
  }
  RECT title{card.left + 20, card.top + 12, card.right - 20, card.top + 40};
  draw_text(
      dc,
      L"進行状況",
      title,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.body_font);

  if (state.clone_running.load()) {
    const auto observed = state.clone_progress.value_or(
        ytec::clonecore::DiskOperationProgress{
            .stage = ytec::clonecore::DiskOperationStage::planning,
            .cancellation_allowed = true,
        });
    const auto progress =
        ytec::windowsapp::build_online_image_progress_view(
            observed, state.clone_elapsed);
    RECT stage{
        card.left + 20, card.top + 45, card.right - 120, card.top + 72};
    draw_text(
        dc,
        progress.stage_label,
        stage,
        progress.cancellation_allowed ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    RECT percent{
        card.right - 112, card.top + 45, card.right - 20, card.top + 72};
    draw_text(
        dc,
        progress.percentage_label,
        percent,
        kSafeGreen,
        DT_RIGHT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);
    RECT track{
        card.left + 20, card.top + 79, card.right - 20, card.top + 94};
    fill_rounded_rect(
        dc, track, RGB(231, 236, 240), RGB(231, 236, 240), 8);
    RECT fill = track;
    fill.right = fill.left + static_cast<LONG>(
        static_cast<double>(fill.right - fill.left) * progress.fraction);
    if (fill.right > fill.left) {
      fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue, 8);
    }
    const int detail_width = (card.right - card.left - 40) / 4;
    const std::array<std::wstring, 4> details{
        L"読込\n" + progress.read_label,
        L"書込\n" + progress.write_label,
        L"検証\n" + progress.verified_label,
        L"残り\n" + progress.remaining_label,
    };
    for (int index = 0; index < 4; ++index) {
      RECT detail{
          card.left + 20 + detail_width * index,
          card.top + 103,
          card.left + 20 + detail_width * (index + 1),
          card.bottom - 8};
      draw_text(
          dc,
          details[static_cast<std::size_t>(index)],
          detail,
          kMuted,
          DT_LEFT | DT_WORDBREAK,
          state.small_font);
    }
    return;
  }

  const auto progress = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::OperationStage::waiting});
  RECT stage{
      card.left + 20, card.top + 45, card.right - 20, card.top + 72};
  draw_text(
      dc,
      progress.stage_label + L"  —  実行開始前",
      stage,
      kMuted,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);
  RECT track{
      card.left + 20, card.top + 79, card.right - 20, card.top + 94};
  fill_rounded_rect(dc, track, RGB(231, 236, 240), RGB(231, 236, 240), 8);

  const int detail_width = (card.right - card.left - 40) / 4;
  const std::array<std::wstring, 4> details{
      L"処理量\n—",
      L"速度\n—",
      L"経過時間\n—",
      L"残り時間\n—"};
  for (int index = 0; index < 4; ++index) {
    RECT detail{
        card.left + 20 + detail_width * index,
        card.top + 103,
        card.left + 20 + detail_width * (index + 1),
        card.bottom - 8};
    draw_text(
        dc,
        details[static_cast<std::size_t>(index)],
        detail,
        kMuted,
        DT_LEFT | DT_WORDBREAK,
        state.small_font);
  }
}

void paint_online_backup_progress(
    const AppState& state,
    HDC dc,
    const RECT& card) {
  const ytec::clonecore::DiskOperationProgress observed =
      state.backup_progress.value_or(
          ytec::clonecore::DiskOperationProgress{
              .stage = ytec::clonecore::DiskOperationStage::planning,
              .cancellation_allowed = true,
          });
  const auto progress =
      ytec::windowsapp::build_online_image_progress_view(
          observed, state.backup_elapsed);

  RECT stage{
      card.left + 26,
      card.top + 116,
      card.right - 150,
      card.top + 144};
  draw_text(
      dc,
      progress.stage_label,
      stage,
      progress.cancellation_allowed ? kTsumugiBlue : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.body_font);
  RECT percentage{
      card.right - 142,
      card.top + 116,
      card.right - 26,
      card.top + 144};
  draw_text(
      dc,
      progress.percentage_label,
      percentage,
      kSafeGreen,
      DT_RIGHT | DT_SINGLELINE | DT_VCENTER,
      state.body_font);

  RECT track{
      card.left + 26,
      card.top + 151,
      card.right - 26,
      card.top + 168};
  fill_rounded_rect(
      dc, track, RGB(230, 235, 240), RGB(230, 235, 240), 9);
  RECT fill = track;
  fill.right =
      fill.left + static_cast<LONG>(
                      static_cast<double>(fill.right - fill.left) *
                      progress.fraction);
  if (fill.right > fill.left) {
    fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue, 9);
  }

  const int inner_left = card.left + 26;
  const int inner_right = card.right - 26;
  constexpr int kGap = 8;
  const int metric_width =
      (inner_right - inner_left - kGap * 3) / 4;
  const int metric_top = card.top + 181;
  const int metric_bottom =
      (std::min)(card.top + 240, card.bottom - 82);
  const std::array<std::wstring_view, 4> labels{
      L"読込", L"書込", L"検証", L"残り時間"};
  const std::array<std::wstring, 4> values{
      progress.read_label,
      progress.write_label,
      progress.verified_label,
      progress.remaining_label};
  const std::array<COLORREF, 4> accents{
      kTsumugiBlue, kTsumugiPurple, kSafeGreen, kWarning};
  const std::array<COLORREF, 4> fills{
      RGB(242, 249, 250),
      RGB(247, 244, 251),
      RGB(243, 250, 247),
      RGB(253, 248, 240)};
  for (int index = 0; index < 4; ++index) {
    const int left =
        inner_left + index * (metric_width + kGap);
    draw_progress_metric(
        state,
        dc,
        RECT{
            left,
            metric_top,
            left + metric_width,
            metric_bottom},
        labels[static_cast<std::size_t>(index)],
        values[static_cast<std::size_t>(index)],
        accents[static_cast<std::size_t>(index)],
        fills[static_cast<std::size_t>(index)]);
  }

  RECT details{
      card.left + 26,
      metric_bottom + 10,
      card.right - 26,
      metric_bottom + 36};
  draw_text(
      dc,
      L"総合処理速度 " + progress.speed_label +
          L"  •  経過 " + progress.elapsed_label +
          (state.backup_cancel_requested.load()
               ? L"  •  安全な停止を待機中"
               : progress.cancellation_allowed
                     ? L"  •  安全な境界で取消可能"
                     : L"  •  最終確定中のため取消不可"),
      details,
      progress.cancellation_allowed ? kMuted : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.small_font);
  RECT safety{
      card.left + 26,
      metric_bottom + 42,
      card.right - 26,
      card.bottom - 16};
  const auto verification_mode =
      selected_image_create_verification_mode(state).value_or(
          ytec::imageformat::TsumugiCreateVerificationMode::complete);
  std::wstring safety_text = selected_image_rescue_mode(state)
      ? L"救出状態: 既保護Sourceを一度だけ読取り  •  所有一時領域を全書込み読戻し後にread-only封印  •  選択済み画像検証と一時領域破棄後だけ完成名へ確定"
      : L"VSS状態: 同一Snapshot Setを使用中  •  アプリはコピー元へ直接書込みなし  •  各書込み読戻し、認証・Hash、最終メタデータ検証後だけ完成名へ確定";
  safety_text += verification_mode ==
          ytec::imageformat::TsumugiCreateVerificationMode::complete
      ? L"  •  完成前の追加全走査あり"
      : L"  •  高速: 完成前の追加全走査のみ省略";
  draw_text(
      dc,
      safety_text,
      safety,
      kMuted,
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_clone_safety(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const auto selection = current_clone_selection(state);
  const bool rescue = state.clone_running.load()
      ? state.active_clone_is_rescue
      : selected_clone_rescue_mode(state);
  const bool shrink = state.clone_running.load()
      ? state.active_clone_is_shrink
      : !rescue &&
      selected_transfer_mode(state) ==
          ytec::imageformat::TransferMode::shrink;
  RECT area{
      306,
      client.bottom - 78,
      client.right - 270,
      client.bottom - 28};
  const std::wstring text = state.clone_running.load()
      ? state.clone_cancel_requested.load()
            ? L"● 安全なチャンク境界で停止し、コピー先をオフラインに保護します。"
            : rescue
                  ? L"● 救出RAWを読取り中。失敗範囲は有限再試行し、残る欠損をゼロ埋めmapへ記録します。"
                  : shrink
                        ? L"● VSSから一時WIMへ縮小再構成中。失敗時はコピー先を未完成・オフラインに保護します。"
                        : L"● VSS Snapshotから読み取り中。コピー元にはアプリから書き込みません。"
      : L"● " +
            (rescue
                 ? current_windows_data_rescue_selection_message(state)
                 : shrink && selection.ready
                       ? L"縮小計画を読取り専用で作成します。一時WIMがコピー先の専用領域に収まらない場合は安全に中止します。"
                       : selection.message);
  const bool ready = rescue
      ? current_windows_data_rescue_selection_ready(state)
      : selection.ready;
  draw_text(
      dc,
      text,
      area,
      state.clone_running.load() || ready ? kSafeGreen : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.small_font);
}

void paint_rescue_media_creation(
    const AppState& state,
    HDC dc,
    const RECT& card) {
  const bool completed = state.media_creation_report.has_value();
  const bool usb =
      completed
          ? state.media_creation_report->complete_usb_verified
          : selected_media_kind(state) ==
                ytec::windowsapp::RescueMediaKind::usb_drive;
  const std::uint8_t percent =
      completed
          ? 100U
          : state.media_creation_progress.has_value()
                ? state.media_creation_progress->percent
                : 0U;
  const std::wstring stage =
      completed
          ? L"作成完了"
          : state.media_creation_progress.has_value()
                ? std::wstring(
                      ytec::windowsapp::rescue_media_creation_stage_label(
                          state.media_creation_progress->stage))
                : L"作成を開始しています";
  const std::wstring message =
      completed
          ? usb
                ? L"USB作成後に全ファイルを読み戻し、SHA-256検証まで完了しました。"
                : L"ISOを完成名へ確定し、全量SHA-256検証まで完了しました。"
          : state.media_creation_progress.has_value()
                ? state.media_creation_progress->message
                : L"製品ファイルとADK環境を再確認しています。";

  RECT stage_area{
      card.left + 26, card.top + 66, card.right - 26, card.top + 98};
  draw_text(
      dc,
      L"手順 4/4  " + stage,
      stage_area,
      completed ? kSafeGreen : kTsumugiPurple,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
      state.body_font);
  RECT message_area{
      card.left + 26, card.top + 101, card.right - 26, card.top + 145};
  draw_text(
      dc,
      message,
      message_area,
      kMuted,
      DT_LEFT | DT_WORDBREAK | DT_VCENTER,
      state.small_font);

  const RECT track{
      card.left + 26, card.top + 157, card.right - 26, card.top + 175};
  fill_rounded_rect(
      dc, track, RGB(230, 235, 240), RGB(230, 235, 240), 9);
  if (percent > 0U) {
    RECT fill = track;
    const LONG width = track.right - track.left;
    fill.right = fill.left +
                 static_cast<LONG>(
                     static_cast<long long>(width) * percent / 100LL);
    fill_rounded_rect(
        dc,
        fill,
        completed ? kSafeGreen : kTsumugiBlue,
        completed ? kSafeGreen : kTsumugiBlue,
        9);
  }

  const ULONGLONG elapsed_ms =
      state.media_creation_started_tick == 0
          ? 0
          : GetTickCount64() - state.media_creation_started_tick;
  const std::wstring elapsed =
      ytec::windowsapp::format_duration(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::milliseconds(elapsed_ms)));
  std::wstring remaining = L"計算中";
  if (completed) {
    remaining = L"0秒";
  } else if (percent > 0U && elapsed_ms >= 1'000U) {
    const ULONGLONG remaining_ms =
        (elapsed_ms / percent) * (100U - percent);
    remaining = ytec::windowsapp::format_duration(
        std::chrono::seconds(
            static_cast<std::chrono::seconds::rep>(
                remaining_ms / 1'000U)));
  }
  const std::array<std::wstring, 3U> details{
      L"進捗\n" + std::to_wstring(percent) + L"%",
      L"経過時間\n" + elapsed,
      L"目安残り時間\n" + remaining,
  };
  const int column_width = (card.right - card.left - 52) / 3;
  for (int index = 0; index < 3; ++index) {
    RECT detail{
        card.left + 26 + column_width * index,
        card.top + 190,
        card.left + 26 + column_width * (index + 1),
        card.top + 239};
    draw_text(
        dc,
        details[static_cast<std::size_t>(index)],
        detail,
        kInk,
        DT_LEFT | DT_WORDBREAK,
        state.small_font);
  }

  RECT result{
      card.left + 20, card.top + 252, card.right - 20, card.bottom - 18};
  fill_rounded_rect(
      dc,
      result,
      completed ? RGB(243, 250, 247) : RGB(247, 248, 252),
      completed ? RGB(178, 220, 199) : kBorder,
      10);
  std::wstring result_text;
  if (completed) {
    const auto& report = state.media_creation_report.value();
    if (report.complete_usb_verified) {
      result_text =
          L"作成先: " + report.usb_root_path +
          L"\nboot.wim SHA-256: " +
          std::wstring(
              report.usb_boot_wim_sha256.begin(),
              report.usb_boot_wim_sha256.end()) +
          L"\n全ファイル検証記録: " + report.manifest_path;
    } else {
      result_text =
          L"保存先: " + report.final_iso_path +
          L"\n容量: " + format_bytes(report.iso_length) +
          L"\nSHA-256: " +
          std::wstring(report.iso_sha256.begin(), report.iso_sha256.end()) +
          L"\n検証記録: " + report.manifest_path;
    }
  } else {
    result_text =
        L"Microsoft製ファイルは配布物へコピーせず、このPCのADKだけを使用します。"
        L"\nWIMのマウント／コミット" +
        std::wstring(usb ? L"とUSB書込み" : L"") +
        L"中は、破損防止のためアプリを終了しないでください。";
  }
  RECT result_text_area{
      result.left + 16,
      result.top + 10,
      result.right - 16,
      result.bottom - 8};
  draw_text(
      dc,
      result_text,
      result_text_area,
      completed ? kSafeGreen : kMuted,
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_rescue_media_page(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const bool usb_selected = selected_media_kind(state) ==
      ytec::windowsapp::RescueMediaKind::usb_drive;
  const auto vertical = ytec::windowsapp::
      calculate_rescue_media_vertical_layout(
          client.bottom, usb_selected);
  const bool compact_height = vertical.compact;
  const RECT card{286, 206, client.right - 36, client.bottom - 92};
  fill_rounded_rect(dc, card, kCard, kBorder);

  RECT title_area{
      card.left + 26,
      card.top + (compact_height ? 8 : 16),
      card.right - 26,
      card.top + (compact_height ? 38 : 50)};
  if (!compact_height || !state.media_preflight.has_value()) {
    draw_text(
        dc,
        L"レスキューUSB／ISO作成ウィザード",
        title_area,
        kInk,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.heading_font);
  }

  if (state.media_creation_running.load() ||
      state.media_creation_report.has_value()) {
    paint_rescue_media_creation(state, dc, card);
    return;
  }

  if (state.adk_management_running.load()) {
    RECT status{
        card.left + 26,
        card.top + 70,
        card.right - 26,
        card.bottom - 24};
    draw_text(
        dc,
        state.adk_management_status,
        status,
        kWarning,
        DT_LEFT | DT_WORDBREAK,
        state.body_font);
    return;
  }

  if (state.media_preflight_running.load()) {
    RECT loading{
        card.left + 26,
        card.top + 70,
        card.right - 26,
        card.bottom - 24};
    draw_text(
        dc,
        L"Microsoft ADK、WinPE Add-on、署名、バージョン、"
        L"必須更新を読み取り専用で確認しています。\n\n"
        L"この確認ではWIM、ISO、USBを作成せず、UACも要求しません。",
        loading,
        kMuted,
        DT_LEFT | DT_WORDBREAK,
        state.body_font);
    return;
  }

  const bool environment_ready =
      state.media_preflight.has_value() &&
      state.media_preflight->media_creation_permitted;
  RECT environment{
      card.left + 26,
      card.top + 53,
      card.right - 26,
      card.top + 85};
  const std::wstring environment_text =
      !state.adk_management_status.empty()
          ? L"● " + state.adk_management_status
          : state.media_preflight.has_value()
          ? environment_ready
                ? L"● 作成環境を確認済み — ADK／WinPE／署名／必須更新: 合格"
                : L"● 作成環境は未完了 — 診断内容を確認して再検査してください"
          : L"● 手順1 — このPCのADK／WinPE作成環境を確認してください";
  if (compact_height && state.media_preflight.has_value()) {
    const auto usb_ui = current_rescue_media_usb_ui_view(state);
    const std::wstring compact_status =
        environment_ready && usb_selected && !usb_ui.status.empty()
        ? usb_ui.status
        : environment_text;
    const bool compact_status_ready =
        environment_ready && (!usb_selected || usb_ui.ready_for_review);
    draw_text(
        dc,
        compact_status,
        title_area,
        compact_status_ready ? kSafeGreen : kWarning,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
  } else {
    draw_text(
        dc,
        environment_text,
        environment,
        environment_ready ? kSafeGreen : kWarning,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
  }

  if (!state.media_preflight.has_value()) {
    RECT introduction{
        card.left + 26,
        card.top + (compact_height ? 88 : 102),
        card.right - 26,
        card.bottom - (compact_height ? 8 : 24)};
    draw_text(
        dc,
        compact_height
            ? L"Microsoft公式のADKとWinPE Add-onを、このPCにインストールして利用します。\n"
              L"製品へMicrosoft製EXE、DLL、WIM、ISOは同梱しません。\n"
              L"不足時は診断後に公式導入・必須更新ページを開けます。最初の確認は読み取り専用です。"
            : L"Microsoft公式のADKとWinPE Add-onを、このPCに"
              L"インストールした状態で利用します。\n\n"
              L"製品にはMicrosoft製EXE、DLL、WIM、ISOを同梱しません。\n\n"
              L"不足している場合は、診断後にMicrosoft公式の導入ページと"
              L"必須更新ページを直接開けます。最初の確認は読み取り専用です。",
        introduction,
        kMuted,
        DT_LEFT | DT_WORDBREAK,
        compact_height ? state.small_font : state.body_font);
    return;
  }

  RECT kind_label{
      card.left + 26,
      vertical.kind_label_top,
      card.left + (card.right - card.left) / 2 - 4,
      vertical.kind_control_top};
  draw_text(
      dc,
      L"作成する種類",
      kind_label,
      kTsumugiBlue,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);
  RECT profile_label{
      card.left + (card.right - card.left) / 2 + 6,
      vertical.kind_label_top,
      card.right - 26,
      vertical.kind_control_top};
  draw_text(
      dc,
      L"起動互換性",
      profile_label,
      kTsumugiPurple,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);

  if (usb_selected) {
    RECT mode_label{
        card.left + 26,
        vertical.option_label_top,
        card.left + (card.right - card.left) / 2 - 4,
        vertical.option_control_top};
    draw_text(
        dc,
        L"USBの更新方法",
        mode_label,
        kWarning,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);
    RECT file_system_label{
        card.left + (card.right - card.left) / 2 + 6,
        vertical.option_label_top,
        card.right - 26,
        vertical.option_control_top};
    draw_text(
        dc,
        L"データ領域の形式",
        file_system_label,
        kTsumugiPurple,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);
  }

  RECT destination_label{
      card.left + 26,
      vertical.destination_label_top,
      card.right - 26,
      vertical.destination_control_top};
  draw_text(
      dc,
      !usb_selected
          ? L"ISOの保存先（既存ファイルは上書きしません）"
          : selected_media_usb_mode(state) ==
                    ytec::windowsapp::
                        RescueUsbProvisioningMode::preserve_data_refresh
              ? L"作成先USB（起動／アプリ領域だけを非上書き更新）"
              : L"作成先USB（全消去→4GiB FAT32＋残容量データ領域）",
      destination_label,
      !usb_selected ? kTsumugiBlue : kWarning,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.small_font);

  const auto plan = current_rescue_media_plan(state);
  if (compact_height) {
    return;
  }
  const RECT result_card{
      card.left + 20,
      card.top + (usb_selected ? 288 : 224),
      card.right - 20,
      card.bottom - 18};
  fill_rounded_rect(
      dc,
      result_card,
      plan.ready_for_confirmation
          ? RGB(243, 250, 247)
          : RGB(253, 248, 241),
      plan.ready_for_confirmation ? RGB(178, 220, 199)
                                  : RGB(232, 205, 166),
      10);
  RECT result_text{
      result_card.left + 16,
      result_card.top + 10,
      result_card.right - 16,
      result_card.bottom - 8};
  std::wstring message = L"● " + plan.message;
  if (plan.ready_for_confirmation) {
    message +=
        L"\n次は作成内容を確認します。まだISO／USBへの書き込みは行いません。";
  } else if (!environment_ready) {
    message +=
        L"\n詳細は診断結果に表示しています。下のボタンから公式導入ガイドを開けます。";
  }
  draw_text(
      dc,
      message,
      result_text,
      plan.ready_for_confirmation ? kSafeGreen : kWarning,
      DT_LEFT | DT_WORDBREAK,
      state.small_font);
}

void paint_non_clone_page(
    const AppState& state,
    HDC dc,
    const RECT& client) {
  const bool full_height_card =
      state.page == Page::boot_repair ||
      state.page == Page::diagnostics;
  const RECT card{
      286,
      full_height_card ? 118 : 206,
      client.right - 36,
      client.bottom - 92};
  fill_rounded_rect(dc, card, kCard, kBorder);

  std::wstring title;
  std::wstring body;
  const auto selected_create_verification =
      selected_image_create_verification_mode(state).value_or(
          ytec::imageformat::TsumugiCreateVerificationMode::complete);
  switch (state.page) {
    case Page::create_image:
      title = L"オンライン・イメージバックアップ";
      if (state.backup_running.load()) {
        body = selected_image_rescue_mode(state)
            ? L"既に保護されたデータディスクを所有一時領域へ救出しています。封印済み一時領域だけから単一 .tsumugi を作成し、選択済み検証・一時領域破棄・保存先再識別後だけ完成名を確定します。"
            : L"同一VSS Snapshot Setから単一 .tsumugi を作成しています。各書込み読戻し、認証・Hash、最終メタデータ検証、BackupCompleteとSnapshot削除後だけ最終ファイル名へ確定します。";
        body += selected_create_verification ==
                ytec::imageformat::TsumugiCreateVerificationMode::complete
            ? L"\n\n完全検証: 完成前の追加全走査も実行します。"
            : L"\n\n高速検証: 完成前の追加全走査だけを省略します。";
        body += L"\n\n完了結果が表示されるまで、このアプリを終了しないでください。";
      } else {
        body = selected_image_rescue_mode(state)
            ? current_windows_data_rescue_image_selection_message(state) +
                  L"\n\n稼働中Windowsのシステムディスクは対象外です。欠損ゼロでも結果分類は救出のままです。"
            : selected_transfer_mode(state) ==
                ytec::imageformat::TransferMode::shrink
            ? L"NTFS領域を同一VSS Snapshot SetからWIMへ取り込み、静的システム領域をexact RAWで保持する単一 .tsumugi を作成します。\n\n現在のWindows直接縮小作成は、BitLocker・exFAT・FAT32の内容領域を開始前に拒否します。縮小復元は安全なオフライン配置Adapterの完成まで停止します。"
            : L"VSSで整合したSnapshotを作成し、保存先の空き容量と同一ディスク誤指定を確認してから単一 .tsumugi を作成します。\n\nWindowsディスクとNTFSデータ専用ディスクに対応し、アプリはコピー元へ直接書き込まず、既存ファイルも上書きしません。";
        body += L"\n\n現在の作成後検証: " +
            std::wstring(image_create_verification_mode_label(
                selected_create_verification));
        body +=
            L"\n\nEXE隣dataとその配下は.tsumugi／隣接.partialの保存先に使用できません。";
      }
      break;
    case Page::restore_image:
      title = L"イメージから復元";
      if (state.restore_running.load()) {
        const auto observed = state.restore_progress.value_or(
            ytec::clonecore::DiskOperationProgress{
                .stage = ytec::clonecore::DiskOperationStage::planning,
                .cancellation_allowed = true,
            });
        const auto progress =
            ytec::windowsapp::build_online_image_progress_view(
                observed, state.restore_elapsed);
        body =
            L"完全検証済み .tsumugi から直接復元しています。\n\n"
            L"段階: " + progress.stage_label +
            L" / " + progress.percentage_label +
            L"\n読込: " + progress.read_label +
            L" / 書込: " + progress.write_label +
            L" / 読戻し検証: " + progress.verified_label +
            L"\n経過: " + progress.elapsed_label +
            L" / 残り: " + progress.remaining_label +
            (state.restore_cancel_requested.load()
                 ? L"\n\n安全な境界での停止を待っています。"
                 : progress.cancellation_allowed
                       ? L"\n\n安全な境界では取消できます。"
                       : L"\n\n最終レイアウト確定中は取消できません。") +
            L"\n復元先は処理後もオフラインのまま保持します。";
      } else if (state.restore_preflight_running.load()) {
        body =
            L"選択した .tsumugi イメージを読み取り専用で完全検証しています。\n\n"
            L"全チャンク、コンテナ全体、認証済みマニフェストと復元構成を照合します。この処理では復元先ディスクを開きません。";
      } else if (state.restore_preflight.has_value()) {
        const auto& report = state.restore_preflight.value();
        const bool shrink =
            report.manifest.mode ==
            ytec::imageformat::TsumugiManifestMode::shrink;
        const bool gpt =
            report.manifest.partition_style ==
            ytec::imageformat::TsumugiManifestPartitionStyle::gpt;
        const bool contains_windows =
            (static_cast<std::uint32_t>(report.manifest.flags) &
             static_cast<std::uint32_t>(
                 ytec::imageformat::TsumugiManifestFlags::
                     source_contains_windows)) != 0U;
        const std::wstring boot_text = contains_windows
            ? (gpt ? L"UEFI" : L"Legacy BIOS")
            : L"起動領域なし（データ専用）";
        const std::size_t payload_count =
            static_cast<std::size_t>(report.header.chunk_count);
        body =
            std::wstring(
                L"イメージの完全検証に合格しました。\n"
                L"モード: ") +
            (shrink ? L"縮小移行モード" : L"通常モード") +
            L"\n"
            L"コピー元容量: " +
            format_bytes(report.header.source_disk_size) +
            L"\n形式: " + (gpt ? L"GPT" : L"MBR") +
            L" / " + boot_text +
            L"\n構成: パーティション " +
            std::to_wstring(report.manifest.partitions.size()) +
            L" / チャンク " + std::to_wstring(payload_count) +
            (report.encrypted ? L" / 暗号化あり" : L" / 暗号化なし") +
            (report.partial_loss ? L" / 一部欠損" : L"");
      } else {
        body =
            L".tsumugiイメージが壊れていないか、"
            L"最初から最後まで読み取り専用で確認します。\n\n"
            L"通常・縮小・救出を同じ単一ファイル形式として識別し、"
            L"Windowsディスクとデータ専用ディスクを復元できます。\n\n"
            L"合格後は復元先を選び、直接復元の安全条件を確認します。"
            L"最終確認で大文字OKが一致するまで復元先ディスクを開きません。\n\n"
            L"全体と分割データのSHA-256、内部構成、容量境界も検査し、"
            L"特殊なリンク、ネットワーク上のファイル、"
            L"デバイス直指定は安全側に拒否します。";
      }
      break;
    case Page::boot_repair:
      title = L"Windowsの起動だけを直す";
      body =
          L"クローンや復元をせず、壊れたWindowsの起動情報（BCD）だけを"
          L"診断・再構築できます。\n\n"
          L"1. このPCでレスキューISOまたはUSBを作る\n"
          L"2. 起動できないPCをそのメディアから起動する\n"
          L"3. 「起動を修復」を選び、対象を確認して実行する\n\n"
          L"UEFI/GPTとレガシーBIOS/MBRを自動判定し、"
          L"Microsoft署名済みのBCDBootだけを使用します。"
          L"パーティションの作成・移動・フォーマットは自動実行しません。";
      break;
    case Page::rescue_media:
      title = L"レスキューUSB／ISO作成";
      if (state.media_preflight_running.load()) {
        body =
            L"Microsoft ADK、WinPE Add-on、Microsoft署名、"
            L"バージョン、必須更新を読み取り専用で確認しています。\n\n"
            L"この操作ではWIM、ISO、USBを作成せず、"
            L"UACも要求しません。";
      } else if (state.media_preflight.has_value()) {
        body = state.media_preflight->status + L"\n\n" +
               state.media_preflight->screen_details;
      } else {
        body =
            L"Microsoft ADKとWinPE Add-onをこのPC上で"
            L"読み取り専用診断します。\n\n"
            L"Microsoft製EXE、DLL、WIM、ISOはリポジトリや"
            L"配布物へ同梱しません。診断ではファイルを変更せず、"
            L"UACも要求しません。\n\n"
            L"実際のISO／USB作成は、UACが必要な確認項目と"
            L"まとめて後ほど実施します。";
      }
      break;
    case Page::diagnostics:
      title = L"PCとディスクの安全診断";
      body =
          (state.elevated
               ? L"現在は管理者権限で起動しています。"
               : L"現在は標準権限で起動しています。") +
          std::wstring(
              L" この画面と「診断情報を更新」はディスクへ書き込みません。");
      if (state.startup_data_policy.diagnostic_only()) {
        body +=
            L"\n\n【診断専用モード】EXE隣のdataを安全に書込み確認できないため、クローン、イメージ作成・復元、USB／ISO作成を停止しています。AppDataへは退避しません。";
        if (!state.startup_data_policy.diagnostic.empty()) {
          body += L"\n理由: " +
                  state.startup_data_policy.diagnostic;
        }
      } else if (
          state.startup_data_policy.intentionally_ram_isolated()) {
        body +=
            L"\n\n保存基盤: 診断ログはbounded RAM内だけに保持（EXE隣dataへ書き込まず、AppDataへも退避しません）";
        if (!state.startup_data_policy.diagnostic.empty()) {
          body += L"\n理由: " + state.startup_data_policy.diagnostic;
        }
      } else {
        body +=
            L"\n\n保存基盤: EXE隣dataの書込み・flush・読戻し・削除確認に合格";
      }
      if (state.inventory_loading.load()) {
        body += L"\n\nディスクを読み取り専用で確認しています…";
      } else if (state.inventory.has_value()) {
        body +=
            L"\n\n検出: " +
            std::to_wstring(state.inventory->disks.size()) +
            L" 台　注意: " +
            std::to_wstring(state.inventory->issues.size()) + L" 件";
        constexpr std::size_t kMaximumVisibleDisks = 5U;
        const std::size_t count =
            (std::min)(
                state.inventory->disks.size(), kMaximumVisibleDisks);
        for (std::size_t index = 0; index < count; ++index) {
          const auto& disk = state.inventory->disks[index];
          body +=
              L"\n• ディスク " +
              std::to_wstring(disk.disk_number) + L"　" +
              (disk.model.empty() ? L"モデル不明" : disk.model) +
              L"　" + format_bytes(disk.size_bytes) + L"　" +
              partition_style_text(disk.partition_style) + L" / " +
              (disk.bus_type.empty() ? L"Bus不明" : disk.bus_type) +
              L"　パーティション " +
              std::to_wstring(disk.partitions.size()) +
              (disk.is_system_disk ? L"　[Windows]" : L"") +
              L"　" + disk_health_summary(disk);
        }
        if (state.inventory->disks.size() > count) {
          body +=
              L"\n• ほか " +
              std::to_wstring(
                  state.inventory->disks.size() - count) +
              L" 台";
        }
      } else {
        body += L"\n\nディスク情報はまだ取得できていません。";
      }
      if (!state.inventory_error.empty()) {
        body +=
            L"\n\n最新の列挙エラー:\n" + state.inventory_error;
      }
      if (!state.log_path.empty()) {
        const std::size_t log_name_separator =
            state.log_path.find_last_of(L"\\/");
        const std::wstring log_name =
            log_name_separator == std::wstring::npos
                ? state.log_path
                : state.log_path.substr(log_name_separator + 1U);
        body +=
            L"\n\n診断ログ（UTF-8／アプリ実行中も閲覧可能）\n"
            L"保存先: アプリと同じフォルダーの「data\\logs」\n"
            L"ログ名: " +
            log_name;
      } else if (!state.log_error.empty()) {
        body +=
            L"\n\n診断ログを作成できませんでした:\n" +
            state.log_error;
      }
      body +=
          L"\n\n安全ガイドは下の「安全ガイド」から、いつでも再表示できます。";
      if (!state.first_run_guidance_status.empty()) {
        body += L"\n" + state.first_run_guidance_status;
      }
      body +=
          L"\n\n「サポートZIP保存」は製品ログを追加マスクし、"
          L"含有ファイル名・マスク後サイズ・選外件数を事前表示します。"
          L"明示確認後に選択したローカル先へ保存するだけで、自動送信しません。";
      if (!state.support_zip_status.empty()) {
        body += L"\nサポートZIP: " + state.support_zip_status;
      }
      if (!state.support_zip_error.empty()) {
        body += L"\nサポートZIP状態: 作成失敗（詳細は再試行時の画面に表示）";
      }
      if (state.support_zip_report.has_value()) {
        body +=
            L"\nサポートZIP状態: ローカル保存・完全検証済み（" +
            std::to_wstring(state.support_zip_report->entries.size()) +
            L"件／" +
            format_bytes(state.support_zip_report->archive_size_bytes) +
            L"）";
      }
      body +=
          L"\n\n更新確認は自動実行しません。「更新を確認」を押した場合だけ、"
          L"固定Y-TEC HTTPSへ16KiB以下の更新情報を問い合わせます。"
          L"更新ファイルの自動ダウンロードや自動実行は行いません。";
      if (state.manual_update_running.load()) {
        body += L"\n更新状態: 利用者操作による確認中…";
      } else if (state.manual_update_report.has_value()) {
        const auto& update = state.manual_update_report.value();
        body += L"\n更新状態: ";
        switch (update.disposition) {
          case ytec::windowsapp::ManualUpdateDisposition::update_available:
            body += L"新しい版 " +
                    widen_ascii(update.manifest.latest_version) +
                    L" を確認";
            break;
          case ytec::windowsapp::ManualUpdateDisposition::up_to_date:
            body += L"現在の版が最新";
            break;
          case ytec::windowsapp::ManualUpdateDisposition::local_version_newer:
            body += L"この内部候補は公開情報より新しい版";
            break;
        }
      } else if (!state.manual_update_error.empty()) {
        body += L"\n更新状態: 確認失敗（詳細は再試行時の画面に表示）";
      }
      break;
    case Page::clone:
      break;
  }

  const bool compact_height = client.bottom < 600;
  if (compact_height && state.page == Page::diagnostics) {
    body = state.elevated
        ? L"管理者権限／診断情報の更新は読み取り専用"
        : L"標準権限／診断情報の更新は読み取り専用";
    body += state.startup_data_policy.diagnostic_only()
        ? L"\n保存基盤: 診断専用（AppDataへ退避しません）"
        : state.startup_data_policy.intentionally_ram_isolated()
              ? L"\n保存基盤: bounded RAM（AppDataへ退避しません）"
              : L"\n保存基盤: EXE隣dataの読戻し確認済み";
    if (state.inventory_loading.load()) {
      body += L"\nディスク: 読み取り専用で確認中…";
    } else if (state.inventory.has_value()) {
      body += L"\nディスク: " +
          std::to_wstring(state.inventory->disks.size()) + L"台／注意 " +
          std::to_wstring(state.inventory->issues.size()) + L"件";
    } else {
      body += L"\nディスク: 未取得";
    }
    body +=
        L"\nサポートZIP: 含有名・追加マスク後サイズ・選外件数を事前表示し、ローカル保存だけ（自動送信なし）";
    if (!state.support_zip_status.empty()) {
      body += L"\n状態: " + state.support_zip_status;
    } else if (state.support_zip_report.has_value()) {
      body += L"\n状態: 完全検証済み";
    }
    body +=
        L"\n更新確認: 利用者が押した時だけ固定Y-TEC HTTPSへ接続";
  } else if (compact_height && state.page == Page::boot_repair) {
    for (std::size_t separator = body.find(L"\n\n");
         separator != std::wstring::npos;
         separator = body.find(L"\n\n")) {
      body.replace(separator, 2, L"\n");
    }
  }

  RECT title_area{
      card.left + 26, card.top + 20, card.right - 26, card.top + 58};
  draw_text(
      dc,
      title,
      title_area,
      kInk,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.heading_font);
  if (state.page == Page::create_image) {
    RECT selector_label{
        card.left + 26,
        card.top + 58,
        card.right - 26,
        card.top + 80};
    draw_text(
        dc,
        L"バックアップ元（Windows／データ専用ディスク・読み取り専用）",
        selector_label,
        kTsumugiBlue,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER,
        state.small_font);
  }
  if (state.page == Page::create_image &&
      state.backup_running.load()) {
    paint_online_backup_progress(state, dc, card);
    return;
  }
  RECT body_area{
      card.left + 26,
      card.top +
          (state.page == Page::create_image
               ? 116
               : compact_height ? 68 : 75),
      card.right - 26,
      state.page == Page::restore_image &&
              state.restore_preflight.has_value()
          ? card.bottom - 230
          : card.bottom - (compact_height ? 8 : 24)};
  draw_text(
      dc,
      body,
      body_area,
      kMuted,
      DT_LEFT | DT_WORDBREAK,
      client.bottom < 640 ||
              (state.page == Page::restore_image &&
               state.restore_preflight.has_value()) ||
              state.page == Page::diagnostics
          ? state.small_font
          : state.body_font);
  if (state.page == Page::restore_image &&
      state.restore_preflight.has_value()) {
    RECT source_selector_label{
        card.left + 26,
        card.bottom - 219,
        card.right - 26,
        card.bottom - 196};
    draw_text(
        dc,
        L"復元範囲（ディスク全体／画像内の個別パーティション）",
        source_selector_label,
        kTsumugiBlue,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    RECT disk_selector_label{
        card.left + 26,
        card.bottom - 155,
        card.right - 26,
        card.bottom - 132};
    draw_text(
        dc,
        L"復元先ディスク（読み取り専用の基礎確認）",
        disk_selector_label,
        kTsumugiBlue,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
        state.small_font);
    if (selected_restore_source_partition(state).has_value()) {
      RECT partition_selector_label{
          card.left + 26,
          card.bottom - 91,
          card.right - 26,
          card.bottom - 68};
      draw_text(
          dc,
          L"復元先（既存区画を上書き／未割当へ新規区画）",
          partition_selector_label,
          kWarning,
          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
          state.small_font);
    }

    const auto target = current_restore_target_selection(state);
    RECT status_area{
        card.left + 26,
        card.bottom - 27,
        card.right - 26,
        card.bottom - 5};
    const std::wstring status = state.restore_running.load()
        ? state.restore_cancel_requested.load()
              ? L"● 安全な境界で停止し、復元先を未完成・オフラインに保護します。"
              : L"● 完全検証後に書込み中。全書込みを読戻し、最後にパーティション表を確定します。"
        : target.ready_for_confirmation
              ? L"● 読み取り専用確認済み。直接復元の安全確認へ進めます。"
              : L"● " + target.message;
    draw_text(
        dc,
        status,
        status_area,
        state.restore_running.load() || target.ready_for_confirmation
            ? kSafeGreen
            : kWarning,
        DT_LEFT | DT_WORDBREAK,
        state.small_font);
  }
}

void paint_window(const AppState& state, HDC dc) {
  RECT client{};
  GetClientRect(state.window, &client);
  const HBRUSH canvas = CreateSolidBrush(kCanvas);
  FillRect(dc, &client, canvas);
  DeleteObject(canvas);
  paint_sidebar(state, dc, client);
  paint_header(state, dc, client);
  if (state.page == Page::clone ||
      state.page == Page::restore_image ||
      state.page == Page::rescue_media) {
    paint_stepper(state, dc, client);
  }
  if (state.page == Page::clone) {
    paint_disk_details(state, dc, client);
    paint_progress_preview(state, dc, client);
    paint_clone_safety(state, dc, client);
  } else if (state.page == Page::rescue_media) {
    paint_rescue_media_page(state, dc, client);
  } else {
    paint_non_clone_page(state, dc, client);
  }
}

void draw_navigation_button(
    const AppState& state,
    const DRAWITEMSTRUCT& item) {
  const int index = static_cast<int>(item.CtlID) - kNavFirstId;
  if (index < 0 ||
      static_cast<std::size_t>(index) >= state.navigation.size()) {
    return;
  }
  const bool selected =
      static_cast<std::size_t>(index) ==
      static_cast<std::size_t>(state.page);
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const COLORREF fill =
      pressed ? RGB(62, 77, 96)
              : selected ? kSidebarSelected : kSidebar;
  const HBRUSH brush = CreateSolidBrush(fill);
  FillRect(item.hDC, &item.rcItem, brush);
  DeleteObject(brush);
  if (selected) {
    RECT accent = item.rcItem;
    accent.right = accent.left + 4;
    const HBRUSH accent_brush = CreateSolidBrush(kTsumugiBlue);
    FillRect(item.hDC, &accent, accent_brush);
    DeleteObject(accent_brush);
  }
  RECT text = item.rcItem;
  text.left += 16;
  draw_text(
      item.hDC,
      kNavigationLabels[static_cast<std::size_t>(index)],
      text,
      selected ? RGB(255, 255, 255) : RGB(190, 205, 216),
      DT_LEFT | DT_SINGLELINE | DT_VCENTER,
      state.body_font);
  if ((item.itemState & ODS_FOCUS) != 0) {
    RECT focus = item.rcItem;
    InflateRect(&focus, -6, -5);
    DrawFocusRect(item.hDC, &focus);
  }
}

LRESULT CALLBACK window_proc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_NCCREATE: {
      const auto* create =
          reinterpret_cast<const CREATESTRUCTW*>(lparam);
      state = static_cast<AppState*>(create->lpCreateParams);
      state->window = window;
      SetWindowLongPtrW(
          window,
          GWLP_USERDATA,
          reinterpret_cast<LONG_PTR>(state));
      return TRUE;
    }
    case WM_CREATE: {
      if (state == nullptr) {
        return -1;
      }
      state->elevated = process_is_elevated();
      state->startup_data_policy =
          ytec::windowsapp::make_read_only_bootstrap_data_policy();
      state->logger = state->log_router.logger();
      state->logger->info(
          L"Y-TEC Tsumugi Drive 起動 version=" +
          widen_ascii(kAppVersion) + L" permission=" +
          (state->elevated ? L"administrator" : L"standard") +
          L" logging=bounded_ram startup_persistent_writes=0");
      const auto windows_version = current_windows_version();
      if (windows_version) {
        state->logger->info(
            L"実行環境 Windows=" +
            std::to_wstring(windows_version.value()[0]) + L"." +
            std::to_wstring(windows_version.value()[1]) + L"." +
            std::to_wstring(windows_version.value()[2]) +
            L" architecture=" +
            (current_native_architecture() == "AMD64"
                 ? L"AMD64"
                 : L"unsupported"));
      } else {
        log_error_summary(
            state->logger,
            L"実行環境バージョン取得失敗",
            windows_version.error());
      }
#if defined(YTEC_UI_ACCEPTANCE_BUILD)
      SetWindowTextW(
          window,
          L"Y-TEC Tsumugi Drive - UI受入（ディスクI/O無効）");
#else
      SetWindowTextW(window, kWindowTitle);
#endif
      const bool line_seed_loaded = state->private_fonts.load_line_seed_jp(
          reinterpret_cast<HMODULE>(
              GetWindowLongPtrW(window, GWLP_HINSTANCE)));
      if (state->logger.has_value()) {
        state->logger->info(
            line_seed_loaded
                ? L"UIフォント LINE Seed JP 読込み成功"
                : L"UIフォント LINE Seed JP 読込み失敗: Yu Gothic UIへフォールバック");
      }
      state->body_font = CreateFontW(
          -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.regular_face());
      state->small_font = CreateFontW(
          -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.regular_face());
      state->heading_font = CreateFontW(
          -25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.bold_face());
      state->brand_font = CreateFontW(
          -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH,
          state->private_fonts.bold_face());
      if (state->body_font == nullptr || state->small_font == nullptr ||
          state->heading_font == nullptr || state->brand_font == nullptr) {
        return -1;
      }

      for (std::size_t index = 0;
           index < state->navigation.size();
           ++index) {
        state->navigation[index] = CreateWindowExW(
            0,
            L"BUTTON",
            kNavigationLabels[index].data(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            0,
            0,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(
                    kNavFirstId + static_cast<int>(index))),
            nullptr,
            nullptr);
      }
      state->refresh = CreateWindowExW(
          0,
          L"BUTTON",
          L"再読み込み",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshId)),
          nullptr,
          nullptr);
      state->source_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
              WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kSourceComboId)),
          nullptr,
          nullptr);
      state->transfer_mode_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kTransferModeComboId)),
          nullptr,
          nullptr);
      state->image_verification_mode_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kImageVerificationModeComboId)),
          nullptr,
          nullptr);
      state->target_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
              WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kTargetComboId)),
          nullptr,
          nullptr);
      state->restore_source_partition_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kRestoreSourcePartitionComboId)),
          nullptr,
          nullptr);
      state->restore_target_partition_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kRestoreTargetPartitionComboId)),
          nullptr,
          nullptr);
      state->restore_change_image = CreateWindowExW(
          0,
          L"BUTTON",
          L"別のイメージを選ぶ",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kRestoreChangeImageId)),
          nullptr,
          nullptr);
      state->media_kind_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaKindComboId)),
          nullptr,
          nullptr);
      state->media_profile_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaProfileComboId)),
          nullptr,
          nullptr);
      state->media_usb_mode_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaUsbModeComboId)),
          nullptr,
          nullptr);
      state->media_usb_file_system_combo = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          WC_COMBOBOXW,
          L"",
          WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaUsbFileSystemComboId)),
          nullptr,
          nullptr);
      state->media_output_edit = CreateWindowExW(
          WS_EX_CLIENTEDGE,
          L"EDIT",
          L"",
          WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaOutputEditId)),
          nullptr,
          nullptr);
      state->media_browse = CreateWindowExW(
          0,
          L"BUTTON",
          L"保存先を選ぶ…",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kMediaBrowseId)),
          nullptr,
          nullptr);
      state->primary_action = CreateWindowExW(
          0,
          L"BUTTON",
          L"安全確認へ",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kPrimaryActionId)),
          nullptr,
          nullptr);
      state->pause_action = CreateWindowExW(
          0,
          L"BUTTON",
          L"一時停止不可",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kPauseActionId)),
          nullptr,
          nullptr);
      state->manual_update_action = CreateWindowExW(
          0,
          L"BUTTON",
          L"更新を確認",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kManualUpdateActionId)),
          nullptr,
          nullptr);
      state->first_run_guidance_action = CreateWindowExW(
          0,
          L"BUTTON",
          L"安全ガイド",
          WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
          0,
          0,
          0,
          0,
          window,
          reinterpret_cast<HMENU>(
              static_cast<INT_PTR>(kFirstRunGuidanceActionId)),
          nullptr,
          nullptr);
      if (state->transfer_mode_combo == nullptr ||
          state->image_verification_mode_combo == nullptr ||
          state->restore_source_partition_combo == nullptr ||
          state->restore_target_partition_combo == nullptr ||
          state->media_kind_combo == nullptr ||
          state->media_profile_combo == nullptr ||
          state->media_usb_mode_combo == nullptr ||
          state->media_usb_file_system_combo == nullptr ||
          state->media_output_edit == nullptr ||
          state->media_browse == nullptr ||
          state->pause_action == nullptr ||
          state->manual_update_action == nullptr ||
          state->first_run_guidance_action == nullptr) {
        return -1;
      }
      constexpr std::array<std::wstring_view, 3> transfer_modes{
          L"通常モード（完全複製）",
          L"縮小移行モード（小容量へ）",
          L"救出モード（データディスク）",
      };
      for (const auto label : transfer_modes) {
        SendMessageW(
            state->transfer_mode_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.data()));
      }
      SendMessageW(state->transfer_mode_combo, CB_SETCURSEL, 0, 0);
      constexpr std::array<std::wstring_view, 2>
          image_verification_modes{
              L"検証: 完全（推奨）",
              L"検証: 高速（完成前の追加全走査のみ省略）",
          };
      for (const auto label : image_verification_modes) {
        SendMessageW(
            state->image_verification_mode_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.data()));
      }
      SendMessageW(
          state->image_verification_mode_combo, CB_SETCURSEL, 0, 0);
      const std::array<ytec::windowsapp::RescueMediaKind, 2>
          media_kinds{
              ytec::windowsapp::RescueMediaKind::iso_file,
              ytec::windowsapp::RescueMediaKind::usb_drive,
          };
      for (const auto kind : media_kinds) {
        const std::wstring label =
            ytec::windowsapp::rescue_media_kind_label(kind);
        SendMessageW(
            state->media_kind_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.c_str()));
      }
      SendMessageW(state->media_kind_combo, CB_SETCURSEL, 0, 0);
      const std::array<
          ytec::windowsapp::RescueMediaBootProfile,
          2>
          media_profiles{
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2011_ca,
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2023_ca,
          };
      for (const auto profile : media_profiles) {
        const std::wstring_view label =
            profile == ytec::windowsapp::RescueMediaBootProfile::
                           windows_uefi_2011_ca
                ? L"2011 CA（互換重視・BIOS／UEFI）"
                : L"2023 CA（最新PC・BIOS／UEFI）";
        SendMessageW(
            state->media_profile_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.data()));
      }
      SendMessageW(state->media_profile_combo, CB_SETCURSEL, 0, 0);
      for (const auto mode : {
               ytec::windowsapp::RescueUsbProvisioningMode::initialize_all,
               ytec::windowsapp::
                   RescueUsbProvisioningMode::preserve_data_refresh,
           }) {
        const auto label = ytec::windowsapp::rescue_usb_ui_mode_label(mode);
        SendMessageW(
            state->media_usb_mode_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.data()));
      }
      SendMessageW(state->media_usb_mode_combo, CB_SETCURSEL, 0, 0);
      for (const auto file_system : {
               ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
               ytec::windowsapp::RescueUsbDataFileSystem::exfat,
           }) {
        const auto label =
            ytec::windowsapp::rescue_usb_ui_file_system_label(file_system);
        SendMessageW(
            state->media_usb_file_system_combo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(label.data()));
      }
      SendMessageW(
          state->media_usb_file_system_combo, CB_SETCURSEL, 0, 0);
      SendMessageW(
          state->media_output_edit, EM_SETLIMITTEXT, 32767, 0);
      for (const HWND button : state->navigation) {
        set_control_font(button, state->body_font);
      }
      set_control_font(state->refresh, state->small_font);
      set_control_font(state->source_combo, state->small_font);
      set_control_font(state->target_combo, state->small_font);
      set_control_font(state->transfer_mode_combo, state->small_font);
      set_control_font(
          state->image_verification_mode_combo, state->small_font);
      set_control_font(
          state->restore_source_partition_combo, state->small_font);
      set_control_font(
          state->restore_target_partition_combo, state->small_font);
      set_control_font(state->restore_change_image, state->body_font);
      set_control_font(state->media_kind_combo, state->small_font);
      set_control_font(state->media_profile_combo, state->small_font);
      set_control_font(state->media_usb_mode_combo, state->small_font);
      set_control_font(
          state->media_usb_file_system_combo, state->small_font);
      set_control_font(state->media_output_edit, state->small_font);
      set_control_font(state->media_browse, state->small_font);
      set_control_font(state->primary_action, state->body_font);
      set_control_font(state->pause_action, state->body_font);
      set_control_font(state->manual_update_action, state->body_font);
      set_control_font(
          state->first_run_guidance_action, state->body_font);
      layout_controls(*state);
      update_action_state(*state);
      start_inventory(*state);
      return 0;
    }
    case WM_SIZE:
      if (state != nullptr) {
        layout_controls(*state);
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      // GDI-scaled 200% turns logical track sizes into twice as many
      // physical pixels. Keep the minimum inside a 1920 x 1080 work area;
      // the content already has compact layouts for this shorter client.
      info->ptMinTrackSize.x = 960;
      info->ptMinTrackSize.y = 516;
      return 0;
    }
    case WM_COMMAND:
      if (state == nullptr) {
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED) {
        const int identifier = LOWORD(wparam);
        if (identifier >= kNavFirstId &&
            identifier <
                kNavFirstId +
                    static_cast<int>(state->navigation.size())) {
          if (state->clone_running.load()) {
            MessageBoxW(
                window,
                L"クローンが安全に完了または停止するまで、この画面で進捗を確認してください。",
                L"ドライブをクローン中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          if (state->backup_running.load()) {
            MessageBoxW(
                window,
                L"イメージ作成が安全に完了または停止するまで、この画面で進捗を確認してください。",
                L"イメージを作成中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          if (state->restore_running.load()) {
            MessageBoxW(
                window,
                L"復元が安全に完了または停止するまで、この画面で進捗を確認してください。",
                L"イメージを復元中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          if (state->media_creation_running.load()) {
            MessageBoxW(
                window,
                L"レスキューメディアの構成が完了するまで、この画面で進捗を確認してください。",
                L"レスキューメディアを作成中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          if (state->support_zip_planning.load() ||
              state->support_zip_creation_running.load()) {
            MessageBoxW(
                window,
                L"サポートZIPが安全に完了するまで、この画面で状態を確認してください。",
                L"サポートZIPを準備・作成中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          if (state->adk_management_running.load()) {
            MessageBoxW(
                window,
                L"ADK取得・管理の確認が完了するまで、この画面で状態を確認してください。",
                L"ADK取得・管理を確認中です",
                MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          state->page =
              static_cast<Page>(identifier - kNavFirstId);
          if (state->page == Page::restore_image &&
              state->restore_preflight.has_value()) {
            select_default_restore_target(*state);
          } else if (
              state->page == Page::rescue_media &&
              selected_media_kind(*state) ==
                  ytec::windowsapp::RescueMediaKind::usb_drive) {
            select_default_media_usb_target(*state);
          }
          update_navigation_state(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, TRUE);
          SetFocus(state->navigation[
              static_cast<std::size_t>(identifier - kNavFirstId)]);
          return 0;
        }
        if (identifier == kRefreshId) {
          if (state->clone_running.load() ||
              state->backup_running.load() ||
              state->restore_running.load() ||
              state->adk_management_running.load() ||
              state->media_usb_inspection_running.load()) {
            return 0;
          }
          start_inventory(*state);
          return 0;
        }
        if (identifier == kManualUpdateActionId &&
            state->page == Page::diagnostics) {
          start_manual_update_check(*state);
          return 0;
        }
        if (identifier == kFirstRunGuidanceActionId &&
            state->page == Page::diagnostics) {
          static_cast<void>(
              show_first_run_guidance_dialog(*state, false));
          return 0;
        }
        if (identifier == kPauseActionId) {
          std::shared_ptr<ytec::clonecore::ManualPauseController>
              controller;
          if (state->page == Page::clone &&
              state->clone_running.load()) {
            controller = state->clone_pause_controller;
          } else if (
              state->page == Page::create_image &&
              state->backup_running.load()) {
            controller = state->backup_pause_controller;
          } else if (
              state->page == Page::restore_image &&
              state->restore_running.load()) {
            controller = state->restore_pause_controller;
          }
          const auto before = controller != nullptr
              ? std::optional(controller->snapshot())
              : std::nullopt;
          if (toggle_manual_pause(controller) &&
              state->logger.has_value() && before.has_value()) {
            state->logger->info(
                before->state ==
                            ytec::clonecore::ManualPauseState::running
                    ? L"手動一時停止を要求"
                    : L"手動一時停止から再開を要求");
          }
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::clone) {
          if (state->clone_running.load()) {
            if (!state->clone_cancel_requested.exchange(true)) {
              if (state->logger.has_value()) {
                state->logger->info(
                    state->active_clone_is_rescue
                        ? L"Windowsデータ救出の安全な取消を要求"
                        : L"Windows直接クローンの安全な取消を要求");
              }
              if (state->clone_pause_controller != nullptr) {
                static_cast<void>(
                    state->clone_pause_controller->request_cancel());
              }
              update_action_state(*state);
              InvalidateRect(window, nullptr, FALSE);
            }
          } else {
            start_online_direct_clone_flow(*state);
          }
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::create_image) {
          if (state->backup_running.load()) {
            state->backup_cancel_requested.store(true);
            if (state->backup_pause_controller != nullptr) {
              static_cast<void>(
                  state->backup_pause_controller->request_cancel());
            }
            if (state->logger.has_value()) {
              state->logger->info(
                  L"オンラインイメージ作成の安全な取消を要求");
            }
            update_action_state(*state);
            InvalidateRect(window, nullptr, FALSE);
          } else {
            create_online_backup_flow(*state);
          }
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::restore_image) {
          if (state->restore_running.load()) {
            if (!state->restore_cancel_requested.exchange(true)) {
              if (state->logger.has_value()) {
                state->logger->info(
                    L"Windows直接.tsumugi復元の安全な取消を要求");
              }
              if (state->restore_pause_controller != nullptr) {
                static_cast<void>(
                    state->restore_pause_controller->request_cancel());
              }
              update_action_state(*state);
              InvalidateRect(window, nullptr, FALSE);
            }
          } else if (state->restore_preflight.has_value()) {
            start_online_image_restore_flow(*state);
          } else {
            create_restore_preflight_flow(*state);
          }
          return 0;
        }
        if (identifier == kRestoreChangeImageId &&
            state->page == Page::restore_image) {
          if (!state->restore_running.load()) {
            create_restore_preflight_flow(*state);
          }
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::boot_repair) {
          state->page = Page::rescue_media;
          if (selected_media_kind(*state) ==
              ytec::windowsapp::RescueMediaKind::usb_drive) {
            select_default_media_usb_target(*state);
          }
          update_navigation_state(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, TRUE);
          SetFocus(state->navigation[
              static_cast<std::size_t>(Page::rescue_media)]);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::rescue_media) {
          if (state->media_creation_running.load()) {
            if (state->media_creation_progress.has_value() &&
                state->media_creation_progress->cancellation_allowed &&
                !state->media_creation_cancel_requested.exchange(true)) {
              if (state->logger.has_value()) {
                state->logger->info(
                    L"レスキューメディア作成の安全な取消を要求");
              }
              update_action_state(*state);
              InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
          }
          if (state->media_creation_report.has_value()) {
            const bool completed_usb =
                state->media_creation_report->complete_usb_verified;
            state->media_creation_report.reset();
            if (completed_usb) {
              start_inventory(*state);
              update_action_state(*state);
              InvalidateRect(window, nullptr, TRUE);
            } else {
              SetWindowTextW(state->media_output_edit, L"");
              choose_media_iso_destination(*state);
            }
          } else if (!state->media_preflight.has_value() ||
              !state->media_preflight->media_creation_permitted) {
            if (state->media_preflight.has_value()) {
              show_adk_management_dialog(*state);
            } else {
              start_media_preflight(*state);
            }
          } else {
            const auto plan = current_rescue_media_plan(*state);
            if (plan.issue ==
                    ytec::windowsapp::RescueMediaPlanIssue::
                        iso_destination_missing ||
                plan.issue ==
                    ytec::windowsapp::RescueMediaPlanIssue::
                        iso_destination_invalid) {
              choose_media_iso_destination(*state);
            } else if (plan.ready_for_confirmation) {
              review_rescue_media_plan(*state);
            }
          }
          return 0;
        }
        if (identifier == kMediaBrowseId &&
            state->page == Page::rescue_media) {
          choose_media_iso_destination(*state);
          return 0;
        }
        if (identifier == kPrimaryActionId &&
            state->page == Page::diagnostics) {
          choose_support_zip_destination(*state);
          return 0;
        }
      }
      if ((LOWORD(wparam) == kSourceComboId ||
           LOWORD(wparam) == kTargetComboId ||
           LOWORD(wparam) == kTransferModeComboId ||
           LOWORD(wparam) == kImageVerificationModeComboId ||
           LOWORD(wparam) == kRestoreSourcePartitionComboId ||
           LOWORD(wparam) == kRestoreTargetPartitionComboId) &&
          HIWORD(wparam) == CBN_SELCHANGE) {
        if (state->page == Page::restore_image &&
            LOWORD(wparam) == kRestoreSourcePartitionComboId) {
          select_default_restore_target(*state);
        } else if (
            state->page == Page::restore_image &&
            LOWORD(wparam) == kTargetComboId) {
          populate_restore_target_partition_candidates(*state);
        } else if (
            state->page == Page::rescue_media &&
            LOWORD(wparam) == kTargetComboId) {
          start_rescue_usb_inspection(*state);
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      if ((LOWORD(wparam) == kMediaKindComboId ||
           LOWORD(wparam) == kMediaProfileComboId ||
           LOWORD(wparam) == kMediaUsbModeComboId ||
           LOWORD(wparam) == kMediaUsbFileSystemComboId) &&
          HIWORD(wparam) == CBN_SELCHANGE) {
        if (LOWORD(wparam) == kMediaKindComboId &&
            selected_media_kind(*state) ==
                ytec::windowsapp::RescueMediaKind::usb_drive) {
          select_default_media_usb_target(*state);
        }
        if (LOWORD(wparam) == kMediaUsbModeComboId &&
            selected_media_usb_mode(*state) == ytec::windowsapp::
                RescueUsbProvisioningMode::preserve_data_refresh &&
            state->media_usb_inspection.has_value() &&
            state->media_usb_inspection->evidence.has_value()) {
          const bool exfat = state->media_usb_inspection->evidence->cache_key
                  .data_file_system ==
              ytec::windowsapp::RescueUsbDataFileSystem::exfat;
          SendMessageW(
              state->media_usb_file_system_combo,
              CB_SETCURSEL,
              exfat ? 1 : 0,
              0);
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      if (LOWORD(wparam) == kMediaOutputEditId &&
          HIWORD(wparam) == EN_CHANGE) {
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case WM_DRAWITEM:
      if (state != nullptr) {
        const auto* item =
            reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item->CtlID >= kNavFirstId &&
            item->CtlID <
                static_cast<UINT>(
                    kNavFirstId + state->navigation.size())) {
          draw_navigation_button(*state, *item);
          return TRUE;
        }
      }
      break;
    case kInventoryCompleteMessage: {
      std::unique_ptr<InventoryPayload> payload(
          reinterpret_cast<InventoryPayload*>(lparam));
      if (state != nullptr) {
        state->inventory_loading.store(false);
        if (state->inventory_thread.joinable()) {
          state->inventory_thread.join();
        }
        state->inventory = std::move(payload->report);
        state->inventory_error = std::move(payload->error);
        EnableWindow(state->refresh, TRUE);
        populate_disk_combos(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kRescueUsbInspectionCompleteMessage: {
      std::unique_ptr<RescueUsbInspectionPayload> payload(
          reinterpret_cast<RescueUsbInspectionPayload*>(lparam));
      if (state != nullptr) {
        state->media_usb_inspection_running.store(false);
        if (state->media_usb_inspection_thread.joinable()) {
          state->media_usb_inspection_thread.join();
        }
        const auto* selected = selected_media_usb_target(*state);
        bool exact_target = false;
        if (selected != nullptr) {
          const auto observed =
              ytec::diskmodel::make_stable_disk_identity(*selected, false);
          exact_target = observed.has_value() &&
              ytec::clonecore::validate_stable_identity(
                  payload->expected_target,
                  observed.value(),
                  L"レスキューUSB検査結果") &&
              ytec::windowsapp::make_rescue_usb_canonical_layout(
                  *selected) == payload->expected_layout;
        }
        if (exact_target) {
          state->media_usb_inspection = std::move(payload->result);
          if (state->media_usb_inspection->state ==
              ytec::windowsapp::RescueUsbInspectionState::verified_owned) {
            SendMessageW(
                state->media_usb_mode_combo, CB_SETCURSEL, 1, 0);
            const bool exfat =
                state->media_usb_inspection->evidence.has_value() &&
                state->media_usb_inspection->evidence->cache_key
                        .data_file_system ==
                    ytec::windowsapp::RescueUsbDataFileSystem::exfat;
            SendMessageW(
                state->media_usb_file_system_combo,
                CB_SETCURSEL,
                exfat ? 1 : 0,
                0);
          } else {
            SendMessageW(
                state->media_usb_mode_combo, CB_SETCURSEL, 0, 0);
            SendMessageW(
                state->media_usb_file_system_combo,
                CB_SETCURSEL,
                0,
                0);
          }
          if (state->logger.has_value()) {
            const auto inspection_state =
                state->media_usb_inspection->state;
            state->logger->info(
                L"レスキューUSB読取り専用所有検査完了 state=" +
                std::wstring(
                    inspection_state == ytec::windowsapp::
                            RescueUsbInspectionState::verified_owned
                        ? L"verified_owned"
                        : inspection_state == ytec::windowsapp::
                                  RescueUsbInspectionState::unknown_media
                            ? L"unknown_media"
                            : L"blocked") +
                L" physical_write_started=false");
          }
        } else {
          state->media_usb_inspection.reset();
        }
        EnableWindow(
            state->refresh,
            state->inventory_loading.load() ? FALSE : TRUE);
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kManualUpdateCompleteMessage: {
      std::unique_ptr<ManualUpdatePayload> payload(
          reinterpret_cast<ManualUpdatePayload*>(lparam));
      if (state != nullptr) {
        state->manual_update_running.store(false);
        if (state->manual_update_thread.joinable()) {
          state->manual_update_thread.join();
        }
        state->manual_update_report = std::move(payload->report);
        state->manual_update_error.clear();
        if (payload->error.has_value()) {
          state->manual_update_error =
              format_error_message(payload->error.value());
          log_error_summary(
              state->logger,
              L"手動更新確認に失敗",
              payload->error.value());
          show_product_error(
              window,
              L"更新情報を確認できませんでした",
              payload->error.value());
        } else if (state->manual_update_report.has_value()) {
          const auto& report = state->manual_update_report.value();
          if (state->logger.has_value()) {
            state->logger->info(
                L"手動更新確認完了 current=" +
                widen_ascii(report.current_version) +
                L" latest=" +
                widen_ascii(report.manifest.latest_version));
          }
          if (report.disposition ==
              ytec::windowsapp::ManualUpdateDisposition::update_available) {
            const std::wstring update_message =
                L"新しい版を確認しました。\n\n現在: " +
                widen_ascii(report.current_version) +
                L"\n公開: " +
                widen_ascii(report.manifest.latest_version) +
                L"\n公開日時: " +
                widen_ascii(report.manifest.published_utc) +
                L"\nSHA-256: " +
                widen_ascii(report.manifest.package_sha256) +
                L"\n\nY-TEC公式配布ページを開きますか？\n"
                L"アプリは更新ファイルを自動取得・実行しません。";
            if (MessageBoxW(
                    window,
                    update_message.c_str(),
                    L"更新があります",
                    MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES) {
              open_manual_update_release_page(*state);
            }
          } else if (
              report.disposition ==
              ytec::windowsapp::ManualUpdateDisposition::up_to_date) {
            MessageBoxW(
                window,
                (L"現在の版 " + widen_ascii(report.current_version) +
                 L" は、公開されている更新情報と一致しています。")
                    .c_str(),
                L"更新はありません",
                MB_OK | MB_ICONINFORMATION);
          } else {
            MessageBoxW(
                window,
                (L"この内部候補 " + widen_ascii(report.current_version) +
                 L" は、公開されている版 " +
                 widen_ascii(report.manifest.latest_version) +
                 L" より新しいため、更新は行いません。")
                    .c_str(),
                L"内部候補を使用中です",
                MB_OK | MB_ICONINFORMATION);
          }
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kSupportZipPlanCompleteMessage: {
      std::unique_ptr<SupportZipPlanPayload> payload(
          reinterpret_cast<SupportZipPlanPayload*>(lparam));
      if (state != nullptr) {
        state->support_zip_planning.store(false);
        if (state->support_zip_thread.joinable()) {
          state->support_zip_thread.join();
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        if (payload->error.has_value()) {
          state->support_zip_error =
              format_error_message(payload->error.value());
          state->support_zip_status = L"含有一覧を安全に準備できませんでした。";
          log_error_summary(
              state->logger,
              L"サポートZIP計画に失敗",
              payload->error.value());
          show_product_error(
              window,
              L"サポートZIPの含有一覧を準備できませんでした",
              payload->error.value());
        } else if (payload->plan.has_value()) {
          auto plan = std::move(payload->plan.value());
          if (review_support_zip_plan(window, state->body_font, plan)) {
            // Do not append to the active product log between planning and
            // creation: the immutable plan intentionally binds its exact
            // File ID, size, timestamps, masked bytes, and CRC.
            start_support_zip_creation(*state, std::move(plan));
          } else {
            state->support_zip_status =
                L"含有一覧の確認でキャンセルしました。ZIPは作成していません。";
          }
        }
        if (!state->support_zip_creation_running.load()) {
          update_action_state(*state);
          InvalidateRect(window, nullptr, TRUE);
        }
      }
      return 0;
    }
    case kSupportZipCreationCompleteMessage: {
      std::unique_ptr<SupportZipCreationPayload> payload(
          reinterpret_cast<SupportZipCreationPayload*>(lparam));
      if (state != nullptr) {
        state->support_zip_creation_running.store(false);
        if (state->support_zip_thread.joinable()) {
          state->support_zip_thread.join();
        }
        if (payload->error.has_value()) {
          state->support_zip_error =
              format_error_message(payload->error.value());
          state->support_zip_status =
              L"ローカルZIPを安全に作成・検証できませんでした。";
          log_error_summary(
              state->logger,
              L"サポートZIP作成に失敗",
              payload->error.value());
          show_product_error(
              window,
              L"サポートZIPを作成できませんでした",
              payload->error.value());
        } else if (payload->report.has_value()) {
          state->support_zip_report = std::move(payload->report);
          state->support_zip_error.clear();
          state->support_zip_status =
              L"ローカル保存と完全検証が完了しました。自動送信していません。";
          const auto& report = state->support_zip_report.value();
          if (state->logger.has_value()) {
            state->logger->info(
                L"サポートZIP作成完了 entries=" +
                std::to_wstring(report.entries.size()) +
                L" archive_bytes=" +
                std::to_wstring(report.archive_size_bytes) +
                L" local_only=true");
          }
          MessageBoxW(
              window,
              (L"サポートZIPをローカル保存し、完全検証しました。\n\n"
               L"含有ログ: " +
               std::to_wstring(report.entries.size()) + L"件\n容量: " +
               format_bytes(report.archive_size_bytes) +
               L"\n自動送信は行っていません。")
                  .c_str(),
              L"サポートZIPを保存しました",
              MB_OK | MB_ICONINFORMATION);
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kManualPauseStateChangedMessage:
      if (state != nullptr) {
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    case kBackupProgressMessage: {
      std::unique_ptr<BackupProgressPayload> payload(
          reinterpret_cast<BackupProgressPayload*>(lparam));
      if (state != nullptr) {
        state->backup_progress = std::move(payload->progress);
        state->backup_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kCloneProgressMessage: {
      std::unique_ptr<CloneProgressPayload> payload(
          reinterpret_cast<CloneProgressPayload*>(lparam));
      if (state != nullptr) {
        state->clone_progress = std::move(payload->progress);
        state->clone_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kRestoreProgressMessage: {
      std::unique_ptr<RestoreProgressPayload> payload(
          reinterpret_cast<RestoreProgressPayload*>(lparam));
      if (state != nullptr) {
        state->restore_progress = std::move(payload->progress);
        state->restore_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kCloneCompleteMessage: {
      std::unique_ptr<ClonePayload> payload(
          reinterpret_cast<ClonePayload*>(lparam));
      if (state != nullptr) {
        state->clone_running.store(false);
        if (state->clone_thread.joinable()) {
          state->clone_thread.join();
        }
        state->clone_pause_controller.reset();
        const bool rescue_completion = payload->rescue_mode;
        const bool shrink_completion = payload->shrink_mode;
        state->active_clone_is_rescue = false;
        state->active_clone_is_shrink = false;
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        bool completion_power_requested = false;
        if (rescue_completion) {
          std::optional<ytec::clonecore::Error> rescue_error =
              payload->error;
          const ytec::windowsapp::
              WindowsDataRescueCloneExecutionReport* rescue_report =
              nullptr;
          if (payload->rescue_report.has_value()) {
            const auto& operation_report =
                payload->rescue_report.value();
            if (operation_report.lifecycle.outcome ==
                    ytec::operationcore::OperationOutcome::completed &&
                operation_report.rescue.has_value()) {
              rescue_report = &operation_report.rescue.value();
            } else if (operation_report.lifecycle.error.has_value()) {
              rescue_error = operation_report.lifecycle.error;
            } else {
              rescue_error = ytec::clonecore::Error{
                  .code =
                      ytec::clonecore::ErrorCode::verification_failed,
                  .native_code = ERROR_CRC,
                  .operation = L"Windowsデータ救出Operation結果",
                  .message = L"完了証跡または救出結果がありません",
              };
            }
          }
          if (rescue_report != nullptr) {
            if (state->logger.has_value()) {
              state->logger->warning(
                  L"Windowsデータ救出検証完了 target_disk=" +
                  std::to_wstring(payload->target_disk_number) +
                  L" recovered_bytes=" +
                  std::to_wstring(rescue_report->raw.copied_source_bytes) +
                  L" zero_filled_bytes=" +
                  std::to_wstring(rescue_report->raw.zero_filled_bytes) +
                  L" missing_ranges=" +
                  std::to_wstring(rescue_report->raw.missing_ranges.size()) +
                  L" target_offline=true");
            }
            const std::wstring result_text =
                L"コピー先: ディスク " +
                std::to_wstring(payload->target_disk_number) +
                L"\r\n\r\n" +
                ytec::windowsapp::
                    format_windows_data_rescue_clone_result(
                        *rescue_report);
            MessageBoxW(
                window,
                result_text.c_str(),
                L"救出コピーを検証しました",
                MB_OK | (rescue_report->raw.partial_data_loss
                              ? MB_ICONWARNING
                              : MB_ICONINFORMATION));
          } else if (rescue_error.has_value()) {
            log_error_summary(
                state->logger,
                L"Windowsデータ救出停止",
                rescue_error.value());
            const bool cancelled =
                rescue_error->code ==
                ytec::clonecore::ErrorCode::cancelled;
            show_product_error(
                window,
                cancelled
                    ? L"救出コピーを安全に取り消しました"
                    : L"救出コピーを完了できませんでした",
                rescue_error.value(),
                L"コピー先はオフラインへ再保護しましたが、ディスクの管理でも状態を確認してください。救出結果は完成扱いにせず、コピー先を使用しないでください。");
          }
          start_inventory(*state);
          return 0;
        }
        if (shrink_completion) {
          std::optional<ytec::clonecore::Error> shrink_error =
              payload->error;
          const ytec::windowsapp::
              WindowsDirectShrinkCloneExecutionReport* shrink_report =
              nullptr;
          if (payload->shrink_report.has_value()) {
            const auto& operation_report = payload->shrink_report.value();
            if (operation_report.lifecycle.outcome ==
                    ytec::operationcore::OperationOutcome::completed &&
                operation_report.execution.has_value()) {
              const auto& execution = operation_report.execution.value();
              if (execution.target_left_offline &&
                  execution.workflow.backup_completed &&
                  execution.workflow.snapshots_deleted &&
                  execution.every_payload_captured_and_applied_inside_snapshot_callback &&
                  execution.snapshots_deleted_before_final_layout_commit &&
                  execution.final_commit.every_write_flushed &&
                  execution.final_commit.every_write_read_back &&
                  execution.final_commit.primary_layout_committed_last &&
                  execution.final_commit.target_offline &&
                  (!execution.boot.required ||
                   (execution.boot.completed &&
                    execution.boot.boot_files_read_back_verified &&
                    execution.boot.recovery_configuration_verified &&
                    execution.boot.target_offline))) {
                shrink_report = &execution;
              } else {
                shrink_error = ytec::clonecore::Error{
                    .code = ytec::clonecore::ErrorCode::verification_failed,
                    .native_code = ERROR_CRC,
                    .operation = L"Windows直接縮小クローン完了証跡",
                    .message = L"Snapshot削除、最終GPT、起動情報、読戻し、またはオフライン証跡が不足しています",
                };
              }
            } else if (operation_report.lifecycle.error.has_value()) {
              shrink_error = operation_report.lifecycle.error;
            } else {
              shrink_error = ytec::clonecore::Error{
                  .code = ytec::clonecore::ErrorCode::verification_failed,
                  .native_code = ERROR_CRC,
                  .operation = L"Windows直接縮小クローンOperation結果",
                  .message = L"完了証跡または縮小実行結果がありません",
              };
            }
          }
          if (shrink_report != nullptr) {
            if (state->logger.has_value()) {
              state->logger->info(
                  L"Windows直接縮小クローン検証完了 target_disk=" +
                  std::to_wstring(payload->target_disk_number) +
                  L" applied_archives=" +
                  std::to_wstring(shrink_report->applied_archive_count) +
                  L" verified_target_bytes=" +
                  std::to_wstring(shrink_report->verified_target_bytes) +
                  L" extended_ntfs_partitions=" +
                  std::to_wstring(
                      shrink_report->final_commit.
                          extended_ntfs_partition_count) +
                  L" target_offline=true");
            }
            const std::wstring completion =
                L"縮小移行、各WIM適用・読戻し、Snapshot削除後の最終GPT公開" +
                std::wstring(
                    shrink_report->boot.required
                        ? L"、起動情報と回復環境の再構築"
                        : L"") +
                L"が完了しました。\n\nコピー先: ディスク " +
                std::to_wstring(payload->target_disk_number) +
                L"\n適用したNTFS: " +
                std::to_wstring(shrink_report->applied_archive_count) +
                L" 領域\n検証済み書込み: " +
                format_bytes(shrink_report->verified_target_bytes) +
                L"\n自動伸長したNTFS: " +
                std::to_wstring(
                    shrink_report->final_commit.
                        extended_ntfs_partition_count) +
                L" 領域\n状態: 検証完了・換装待ち（オフライン）"
                L"\n\n実機での起動成功を確認した表示ではありません。"
                L"アプリを終了し、換装後に起動確認を行ってください。";
            MessageBoxW(
                window,
                completion.c_str(),
                L"縮小移行を検証しました・換装待ち",
                MB_OK | MB_ICONINFORMATION);
            const auto proof = ytec::windowsapp::
                make_direct_shrink_clone_completion_power_proof(
                    payload->shrink_report.value(),
                    payload->sleep_prevention_release,
                    payload->completion_power_operation_binding);
            completion_power_requested = offer_completion_power_action(
                *state, proof, L"縮小移行クローン");
          } else if (shrink_error.has_value()) {
            log_error_summary(
                state->logger,
                L"Windows直接縮小クローン停止",
                shrink_error.value());
            const bool cancelled =
                shrink_error->code ==
                ytec::clonecore::ErrorCode::cancelled;
            show_product_error(
                window,
                cancelled
                    ? L"縮小移行を安全に取り消しました"
                    : L"縮小移行を完了できませんでした",
                shrink_error.value(),
                L"コピー先は未完成としてオフラインへ再保護しました。Windowsの「ディスクの管理」でもオフラインを確認し、確認できるまで内容を使用しないでください。");
          }
          if (!completion_power_requested) {
            start_inventory(*state);
          }
          return 0;
        }
        std::optional<ytec::clonecore::Error> completion_error =
            payload->error;
        const ytec::windowsapp::OnlineDirectCloneReport* clone_report =
            nullptr;
        if (payload->report.has_value()) {
          const auto& operation_report = payload->report.value();
          if (operation_report.lifecycle.outcome ==
                  ytec::operationcore::OperationOutcome::completed &&
              operation_report.clone.has_value()) {
            clone_report = &operation_report.clone.value();
          } else if (operation_report.lifecycle.error.has_value()) {
            completion_error = operation_report.lifecycle.error;
          } else {
            completion_error = ytec::clonecore::Error{
                .code = ytec::clonecore::ErrorCode::verification_failed,
                .native_code = ERROR_CRC,
                .operation = L"Windows直接クローンOperation結果",
                .message = L"完了証跡またはクローン結果がありません",
            };
          }
        }
        if (clone_report != nullptr) {
          const auto& report = *clone_report;
          if (state->logger.has_value()) {
            state->logger->info(
                L"Windows直接クローン検証完了 target_disk=" +
                std::to_wstring(payload->target_disk_number) +
                L" copied_bytes=" +
                std::to_wstring(report.copied_data_bytes) +
                L" target_offline=" +
                (report.target_left_offline ? L"true" : L"false") +
                L" boot_finalization_required=" +
                (report.boot_finalization_required ? L"true" : L"false") +
                L" boot_finalized=" +
                (report.boot_finalization_completed ? L"true" : L"false"));
          }
          const std::wstring completion =
              L"ドライブのクローンと読戻し検証" +
              std::wstring(
                  report.boot_finalization_required
                      ? L"、起動情報の新規再構築"
                      : L"") +
              L"が完了しました。\n\n"
              L"コピー先: ディスク " +
              std::to_wstring(payload->target_disk_number) +
              L"\n検証済みデータ: " +
              format_bytes(report.copied_data_bytes) +
              L"\n状態: 検証完了・換装待ち（オフライン）"
              L"\n\n実機での起動成功を確認した表示ではありません。"
              L"アプリを終了し、換装後に起動確認を行ってください。";
          MessageBoxW(
              window,
              completion.c_str(),
              L"検証完了・換装待ち",
              MB_OK | MB_ICONINFORMATION);
          const auto proof =
              ytec::windowsapp::make_clone_completion_power_proof(
                  payload->report.value(),
                  payload->sleep_prevention_release,
                  payload->completion_power_operation_binding);
          completion_power_requested = offer_completion_power_action(
              *state, proof, L"ドライブのクローン");
        } else if (completion_error.has_value()) {
          log_error_summary(
              state->logger,
              L"Windows直接クローン停止",
              completion_error.value());
          const bool cancelled =
              completion_error->code ==
              ytec::clonecore::ErrorCode::cancelled;
          show_product_error(
              window,
              cancelled
                  ? L"クローンを安全に取り消しました"
                  : L"クローンを完了できませんでした",
              completion_error.value(),
              L"書込みが開始されていた可能性があります。コピー先がオフラインかWindowsの「ディスクの管理」で確認してください。オフラインを確認できるまでコピー先の内容を使用しないでください。");
        }
        if (!completion_power_requested) {
          start_inventory(*state);
        }
      }
      return 0;
    }
    case kBackupCompleteMessage: {
      std::unique_ptr<BackupPayload> payload(
          reinterpret_cast<BackupPayload*>(lparam));
      if (state != nullptr) {
        state->backup_running.store(false);
        if (state->backup_thread.joinable()) {
          state->backup_thread.join();
        }
        state->backup_pause_controller.reset();
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        if (payload->rescue_report.has_value()) {
          const auto& report = payload->rescue_report.value();
          const auto verification_mode =
              report.rescue.image.stream.verification_mode;
          if (state->logger.has_value()) {
            state->logger->warning(
                L"Windowsデータ救出.tsumugi作成完了 image_bytes=" +
                std::to_wstring(report.rescue.image.stream.image_length) +
                L" missing_ranges=" +
                std::to_wstring(report.rescue.rescue.missing_ranges.size()) +
                L" zero_filled_bytes=" +
                std::to_wstring(report.rescue.rescue.zero_filled_bytes) +
                L" classification=rescue verification=" +
                (verification_mode == ytec::imageformat::
                         TsumugiCreateVerificationMode::complete
                     ? L"complete"
                     : L"fast"));
          }
          const std::wstring completion_text =
              L"救出 .tsumugi の作成、各書込み読戻し、認証・Hash、最終メタデータ検証、所有一時領域の破棄、保存先再識別が完了しました。\n\n" +
              payload->final_path +
              L"\n\nイメージ容量: " +
              format_bytes(report.rescue.image.stream.image_length) +
              L"\n欠損範囲: " +
              std::to_wstring(report.rescue.rescue.missing_ranges.size()) +
              L" 件\nゼロ埋め: " +
              format_bytes(report.rescue.rescue.zero_filled_bytes) +
              L"\n検証方式: " +
              std::wstring(image_create_verification_mode_label(
                  verification_mode)) +
              L"\n完成前の追加全走査: " +
              (verification_mode == ytec::imageformat::
                       TsumugiCreateVerificationMode::complete
                   ? L"合格"
                   : L"高速検証のため省略") +
              L"\n結果分類: 救出（一部欠損の可能性あり）"
              L"\n\n欠損が0件でも通常成功へは読み替えていません。復元前に欠損mapと対象データを確認してください。";
          MessageBoxW(
              window,
              completion_text.c_str(),
              L"救出イメージを検証しました",
              MB_OK | MB_ICONWARNING);
        } else if (payload->report.has_value()) {
          const auto& report = payload->report.value();
          const auto verification_mode =
              report.image.stream.verification_mode;
          if (state->logger.has_value()) {
            state->logger->info(
                L"オンライン.tsumugi作成完了 image_bytes=" +
                std::to_wstring(report.image.stream.image_length) +
                L" final_committed_after_vss=" +
                (report.final_file_committed_after_vss
                     ? L"true"
                     : L"false") +
                L" verification=" +
                (verification_mode == ytec::imageformat::
                         TsumugiCreateVerificationMode::complete
                     ? L"complete"
                     : L"fast"));
          }
          const std::wstring completion_text =
              L"オンライン .tsumugi イメージを作成し、各書込み読戻し、認証・Hash、最終メタデータを検証しました。\n\n" +
              payload->final_path +
              L"\n\nイメージ容量: " +
              format_bytes(report.image.stream.image_length) +
              L"\n保存データ: " +
              format_bytes(report.image.stream.stored_data_bytes) +
              L"\n検証方式: " +
              std::wstring(image_create_verification_mode_label(
                  verification_mode)) +
              L"\n完成前の追加全走査: " +
              (verification_mode == ytec::imageformat::
                       TsumugiCreateVerificationMode::complete
                   ? L"合格"
                   : L"高速検証のため省略") +
              L"\nVSS Snapshot削除後の確定: " +
              (report.final_file_committed_after_vss ? L"完了" : L"未完了");
          MessageBoxW(
              window,
              completion_text.c_str(),
              kWindowTitle,
              MB_OK | MB_ICONINFORMATION);
          const auto proof =
              ytec::windowsapp::make_image_create_completion_power_proof(
                  report,
                  payload->rescue_mode,
                  payload->sleep_prevention_release,
                  payload->completion_power_operation_binding);
          static_cast<void>(offer_completion_power_action(
              *state, proof, L"イメージ作成"));
        } else if (payload->error.has_value()) {
          log_error_summary(
              state->logger,
              payload->error->native_code == ERROR_CANCELLED
                  ? payload->rescue_mode
                        ? L"Windowsデータ救出イメージ作成キャンセル"
                        : L"オンラインイメージ作成キャンセル"
                  : payload->rescue_mode
                        ? L"Windowsデータ救出イメージ作成失敗"
                        : L"オンラインイメージ作成失敗",
              payload->error.value());
          show_product_error(
              window,
              kWindowTitle,
              payload->error.value());
        }
      }
      return 0;
    }
    case kRestoreCompleteMessage: {
      std::unique_ptr<RestorePayload> payload(
          reinterpret_cast<RestorePayload*>(lparam));
      if (state != nullptr) {
        state->restore_running.store(false);
        if (state->restore_thread.joinable()) {
          state->restore_thread.join();
        }
        state->restore_pause_controller.reset();
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        bool completion_power_requested = false;

        const bool exact_completed =
            payload->report.has_value() &&
            payload->report->lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::completed &&
            payload->report->restore.has_value();
        const bool shrink_completed =
            payload->shrink_report.has_value() &&
            payload->shrink_report->lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::completed &&
            payload->shrink_report->restore.has_value();
        if (exact_completed) {
          const auto& report = payload->report->restore.value();
          if (state->logger.has_value()) {
            state->logger->info(
                L"Windows直接.tsumugi復元検証完了 target_disk=" +
                std::to_wstring(payload->target_disk_number) +
                L" written_bytes=" +
                std::to_wstring(report.restore.written_logical_bytes) +
                L" target_offline=true");
          }
          std::wstring completion =
              std::wstring(payload->individual_partition
                              ? L"選択したパーティションの復元と全書込みの読戻し検証が完了しました。パーティション表と他の区画は変更していません。\n\n"
                              : L"イメージの復元、全書込みの読戻し検証、パーティション表の最終確定が完了しました。\n\n") +
              L"復元先: ディスク " +
              std::to_wstring(payload->target_disk_number) +
              L"\n検証済みデータ: " +
              format_bytes(report.restore.written_logical_bytes) +
              L"\n状態: 検証完了・オフライン保持";
          if (report.partial_loss) {
            completion +=
                L"\n結果: 一部欠損（救出イメージの欠損範囲はゼロ埋め）";
          }
          if (report.boot_repair_offer_required) {
            completion +=
                L"\n\nWindowsを含むため、換装後に起動しない場合はレスキューPEの「起動を修復」で対象ディスクを自動診断してください。";
          }
          completion +=
              L"\n\nこの表示は実機での起動成功を確認したものではありません。";
          MessageBoxW(
              window,
              completion.c_str(),
              L"復元と検証が完了しました",
              MB_OK | MB_ICONINFORMATION);
          const auto proof = ytec::windowsapp::
              make_exact_restore_completion_power_proof(
                  payload->report.value(),
                  payload->sleep_prevention_release,
                  payload->completion_power_operation_binding);
          completion_power_requested = offer_completion_power_action(
              *state, proof, L"イメージの復元");
        } else if (shrink_completed) {
          const auto& report =
              payload->shrink_report->restore.value();
          const std::uint64_t verified_bytes =
              report.restore.archive_logical_bytes +
              report.restore.exact_raw_logical_bytes +
              report.restore.intentionally_omitted_logical_bytes;
          if (state->logger.has_value()) {
            state->logger->info(
                L"Windows縮小.tsumugi復元検証完了 target_disk=" +
                std::to_wstring(payload->target_disk_number) +
                L" verified_payload_bytes=" +
                std::to_wstring(verified_bytes) +
                L" archive_partitions=" +
                std::to_wstring(
                    report.restore.completed_archive_partitions) +
                L" target_offline=true");
          }
          std::wstring completion =
              L"縮小イメージの復元、WIM適用後の全通常ファイル読戻し、exact RAW読戻し、最終パーティション表の確定が完了しました。\n\n"
              L"復元先: ディスク " +
              std::to_wstring(payload->target_disk_number) +
              L"\n検証済みpayload: " + format_bytes(verified_bytes) +
              L"\nWIM適用済み区画: " +
              std::to_wstring(
                  report.restore.completed_archive_partitions) +
              L"\n空filesystem再作成区画: " +
              std::to_wstring(
                  report.restore.completed_empty_file_system_partitions) +
              L"\n状態: 検証完了・オフライン保持";
          if (report.boot_repair_offer_required) {
            completion +=
                L"\n\nWindowsを含むため、換装後に起動しない場合はレスキューPEの「起動を修復」で対象ディスクを自動診断してください。";
          }
          completion +=
              L"\n\nこの表示は実機での起動成功を確認したものではありません。";
          MessageBoxW(
              window,
              completion.c_str(),
              L"縮小復元と検証が完了しました",
              MB_OK | MB_ICONINFORMATION);
          const auto proof = ytec::windowsapp::
              make_shrink_restore_completion_power_proof(
                  payload->shrink_report.value(),
                  payload->sleep_prevention_release,
                  payload->completion_power_operation_binding);
          completion_power_requested = offer_completion_power_action(
              *state, proof, L"縮小イメージの復元");
        } else {
          std::optional<ytec::clonecore::Error> error = payload->error;
          if (!error.has_value() && payload->report.has_value() &&
              payload->report->lifecycle.error.has_value()) {
            error = payload->report->lifecycle.error;
          }
          if (!error.has_value() && payload->shrink_report.has_value() &&
              payload->shrink_report->lifecycle.error.has_value()) {
            error = payload->shrink_report->lifecycle.error;
          }
          if (error.has_value()) {
            log_error_summary(
                state->logger,
                L"Windows直接.tsumugi復元停止",
                error.value());
            const bool cancelled =
                error->code == ytec::clonecore::ErrorCode::cancelled;
            show_product_error(
                window,
                cancelled
                    ? L"復元を安全に取り消しました"
                    : L"イメージを復元できませんでした",
                error.value(),
                L"書込み開始後に停止した場合、復元先は未完成・オフラインのまま保護しています。");
          } else {
            MessageBoxW(
                window,
                L"復元の完了証跡を確認できません。復元先はオフラインのまま扱ってください。",
                L"イメージを復元できませんでした",
                MB_OK | MB_ICONERROR);
          }
        }
        if (!completion_power_requested) {
          start_inventory(*state);
        }
      }
      return 0;
    }
    case kMediaPreflightCompleteMessage: {
      std::unique_ptr<MediaPreflightPayload> payload(
          reinterpret_cast<MediaPreflightPayload*>(lparam));
      if (state != nullptr) {
        state->media_preflight_running.store(false);
        if (state->media_preflight_thread.joinable()) {
          state->media_preflight_thread.join();
        }
        state->media_preflight = std::move(payload->view);
        if (state->logger.has_value()) {
          const auto& view = state->media_preflight.value();
          state->logger->info(
              L"レスキューメディア作成環境診断完了 permitted=" +
              std::wstring(
                  view.media_creation_permitted ? L"true" : L"false") +
              L" base_layout=" +
              (view.base_layout_ready ? L"true" : L"false") +
              L" bootex_layout=" +
              (view.bootex_layout_ready ? L"true" : L"false"));
        }
        if (selected_media_kind(*state) ==
            ytec::windowsapp::RescueMediaKind::usb_drive) {
          select_default_media_usb_target(*state);
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        const UINT icon =
            state->media_preflight->media_creation_permitted
                ? MB_ICONINFORMATION
                : MB_ICONWARNING;
        const std::wstring message_text =
            state->media_preflight->status + L"\n\n" +
            state->media_preflight->details +
            L"\n\nこの確認ではメディアを作成していません。";
        MessageBoxW(
            window,
            message_text.c_str(),
            L"レスキューメディア作成前診断",
            MB_OK | icon);
      }
      return 0;
    }
    case kAdkManagementCompleteMessage: {
      std::unique_ptr<AdkManagementPayload> payload(
          reinterpret_cast<AdkManagementPayload*>(lparam));
      if (state != nullptr) {
        state->adk_management_running.store(false);
        if (state->adk_management_thread.joinable()) {
          state->adk_management_thread.join();
        }
        if (payload->report.has_value()) {
          const auto& report = payload->report.value();
          if (report.offline_layout_completed) {
            state->adk_management_status =
                L"検証済みADKオフラインレイアウトを作成しました。";
          } else if (report.managed_record_removed) {
            state->adk_management_status =
                L"Tsumugiが管理していたADK登録だけを削除しました。";
          } else if (report.installed_by_this_operation) {
            state->adk_management_status =
                L"Microsoft公式取得物を検証・導入し、管理記録を保存しました。";
          } else {
            state->adk_management_status =
                L"検証済みの既存ADKを維持しました。";
          }
          if (state->logger.has_value()) {
            state->logger->info(
                L"ADK取得・管理の明示操作完了 manifest=" +
                std::wstring(
                    report.manifest_id.begin(), report.manifest_id.end()));
          }
          MessageBoxW(
              window,
              state->adk_management_status.c_str(),
              L"ADK取得・管理を完了しました",
              MB_OK | MB_ICONINFORMATION);
          start_media_preflight(*state);
        } else if (payload->error.has_value()) {
          const auto& error = payload->error.value();
          const auto view = ytec::windowsapp::build_adk_management_view(
              ytec::windowsapp::tsumugi_1_0_0_adk_manifest());
          if (!view.execution_gate_open) {
            state->adk_management_status =
                L"安全ゲート停止中: 通信・フォルダー参照・UAC・installerは開始していません。";
            if (state->logger.has_value()) {
              state->logger->info(
                  L"ADK取得・管理はrelease安全ゲートで停止（外部処理なし）");
            }
            MessageBoxW(
                window,
                (state->adk_management_status + L"\n\n" + error.message)
                    .c_str(),
                L"ADK取得・管理は安全側に停止しました",
                MB_OK | MB_ICONINFORMATION);
          } else {
            state->adk_management_status =
                L"ADK取得・管理を完了できませんでした。";
            log_error_summary(
                state->logger,
                L"ADK取得・管理失敗",
                error);
            show_product_error(
                window,
                L"ADK取得・管理を完了できませんでした",
                error);
          }
        } else {
          state->adk_management_status =
              L"ADK取得・管理結果を受信できませんでした。";
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case kMediaCreationProgressMessage: {
      std::unique_ptr<MediaCreationProgressPayload> payload(
          reinterpret_cast<MediaCreationProgressPayload*>(lparam));
      if (state != nullptr) {
        if (state->logger.has_value() &&
            (!state->last_logged_media_stage.has_value() ||
             state->last_logged_media_stage.value() !=
                 payload->progress.stage)) {
          state->last_logged_media_stage = payload->progress.stage;
          state->logger->info(
              L"レスキューメディア作成進捗 stage=" +
              std::wstring(
                  ytec::windowsapp::
                      rescue_media_creation_stage_label(
                          payload->progress.stage)) +
              L" percent=" +
              std::to_wstring(payload->progress.percent));
        }
        state->media_creation_progress =
            std::move(payload->progress);
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kMediaCreationCompleteMessage: {
      std::unique_ptr<MediaCreationPayload> payload(
          reinterpret_cast<MediaCreationPayload*>(lparam));
      if (state != nullptr) {
        state->media_creation_running.store(false);
        KillTimer(window, kUiRefreshTimerId);
        if (state->media_creation_thread.joinable()) {
          state->media_creation_thread.join();
        }
        if (state->manual_update_thread.joinable()) {
          state->manual_update_thread.join();
        }
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        if (payload->report.has_value()) {
          state->media_creation_report =
              std::move(payload->report.value());
          const auto& report = state->media_creation_report.value();
          const bool usb =
              payload->requested_kind ==
              ytec::windowsapp::RescueMediaKind::usb_drive;
          if (state->logger.has_value()) {
            if (usb) {
              state->logger->info(
                  L"レスキューUSB作成・全ファイル検証完了 root=" +
                  report.usb_root_path + L" boot_wim_sha256=" +
                  std::wstring(
                      report.usb_boot_wim_sha256.begin(),
                      report.usb_boot_wim_sha256.end()));
            } else {
              state->logger->info(
                  L"レスキューISO作成・全量検証完了 iso_bytes=" +
                  std::to_wstring(report.iso_length) + L" sha256=" +
                  std::wstring(
                      report.iso_sha256.begin(),
                      report.iso_sha256.end()));
            }
          }
          const std::wstring completion = usb
              ? L"レスキューUSBを作成し、全ファイルをSHA-256で"
                L"読み戻し検証しました。\n\n作成先: " +
                    report.usb_root_path +
                    L"\nboot.wim SHA-256:\n" +
                    std::wstring(
                        report.usb_boot_wim_sha256.begin(),
                        report.usb_boot_wim_sha256.end()) +
                    L"\n\n検証記録:\n" + report.manifest_path +
                    L"\n\n一時作業フォルダー:\n" +
                    report.retained_work_root +
                    L"\n現在は監査用に保持しています。"
              : L"レスキューISOを作成し、全量SHA-256検証しました。\n\n" +
                    report.final_iso_path +
                    L"\n\n容量: " + format_bytes(report.iso_length) +
                    L"\nSHA-256:\n" +
                    std::wstring(
                        report.iso_sha256.begin(),
                        report.iso_sha256.end()) +
                    L"\n\n検証記録:\n" + report.manifest_path +
                    L"\n\n一時作業フォルダー:\n" +
                    report.retained_work_root +
                    L"\n現在は監査用に保持しています。";
          MessageBoxW(
              window,
              completion.c_str(),
              usb ? L"レスキューUSBの作成完了"
                  : L"レスキューISOの作成完了",
              MB_OK | MB_ICONINFORMATION);
          const auto proof = ytec::windowsapp::
              make_rescue_media_completion_power_proof(
                  report,
                  payload->requested_kind,
                  payload->sleep_prevention_release,
                  payload->completion_power_operation_binding);
          static_cast<void>(offer_completion_power_action(
              *state,
              proof,
              usb ? L"レスキューUSBの作成"
                  : L"レスキューISOの作成"));
        } else if (payload->error.has_value() &&
                   payload->error->native_code != ERROR_CANCELLED) {
          log_error_summary(
              state->logger,
              L"レスキューメディア作成失敗",
              payload->error.value());
          const bool usb =
              selected_media_kind(*state) ==
              ytec::windowsapp::RescueMediaKind::usb_drive;
          show_product_error(
              window,
              usb ? L"レスキューUSBを作成できませんでした"
                  : L"レスキューISOを作成できませんでした",
              payload->error.value(),
              usb
                  ? L"完了扱いにはしていません。USBと監査ログを確認してください。"
                  : L"既存ISOは上書きしていません。途中作業がある場合は選択した保存先とログを確認してください。");
        } else if (payload->error.has_value()) {
          log_error_summary(
              state->logger,
              L"レスキューメディア作成キャンセル",
              payload->error.value());
        }
      }
      return 0;
    }
    case kRestorePreflightCompleteMessage: {
      std::unique_ptr<RestorePreflightPayload> payload(
          reinterpret_cast<RestorePreflightPayload*>(lparam));
      if (state != nullptr) {
        state->restore_preflight_running.store(false);
        if (state->restore_preflight_thread.joinable()) {
          state->restore_preflight_thread.join();
        }
        if (payload->report.has_value()) {
          state->restore_preflight =
              std::move(payload->report.value());
          populate_restore_source_partition_candidates(*state);
          select_default_restore_target(*state);
          update_action_state(*state);
          const auto& report = state->restore_preflight.value();
          const bool shrink =
              report.manifest.mode ==
              ytec::imageformat::TsumugiManifestMode::shrink;
          const std::size_t payload_count =
              static_cast<std::size_t>(report.header.chunk_count);
          if (state->logger.has_value()) {
            state->logger->info(
                L".tsumugi復元イメージの読み取り専用完全検証完了 image_bytes=" +
                std::to_wstring(report.image_length) +
                L" partitions=" +
                std::to_wstring(report.manifest.partitions.size()) +
                L" chunks=" +
                std::to_wstring(payload_count));
          }
          const std::wstring completion_text =
              std::wstring(
                  shrink
                      ? L"縮小移行 .tsumugi の全チャンク・マニフェスト検証に合格しました。\n\n"
                      : L"通常 .tsumugi の読み取り専用完全検証に合格しました。\n\n") +
              report.canonical_path +
              L"\n\nイメージ容量: " +
              format_bytes(report.image_length) +
              L"\n元ディスク容量: " +
              format_bytes(report.header.source_disk_size) +
              L"\nパーティション: " +
              std::to_wstring(report.manifest.partitions.size()) +
              L"\nチャンク: " + std::to_wstring(payload_count) +
              (report.encrypted ? L"\n暗号化: あり" : L"\n暗号化: なし") +
              (report.partial_loss
                   ? L"\n状態: 一部欠損（救出イメージ）"
                   : L"\n状態: 完全") +
              L"\n\n復元先ディスクは開いていません。"
              L"\n次に復元先を選び、消去内容と大文字OKを確認します。";
          MessageBoxW(
              window,
              completion_text.c_str(),
              L"イメージ復元前の完全検証",
              MB_OK | MB_ICONINFORMATION);
        } else if (payload->error.has_value() &&
                   payload->error->native_code != ERROR_CANCELLED) {
          log_error_summary(
              state->logger,
              L"復元イメージの読み取り専用完全検証失敗",
              payload->error.value());
          update_action_state(*state);
          show_product_error(
              window,
              L"イメージを検証できませんでした",
              payload->error.value());
        } else {
          if (payload->error.has_value()) {
            log_error_summary(
                state->logger,
                L"復元イメージの読み取り専用完全検証キャンセル",
                payload->error.value());
          }
          update_action_state(*state);
        }
        InvalidateRect(window, nullptr, TRUE);
      }
      return 0;
    }
    case WM_TIMER:
      if (state != nullptr &&
          wparam == kCloneCompletionFallbackTimerId &&
          state->clone_completion_post_failed.exchange(false)) {
        KillTimer(window, kCloneCompletionFallbackTimerId);
        state->clone_running.store(false);
        if (state->clone_thread.joinable()) {
          state->clone_thread.join();
        }
        state->clone_pause_controller.reset();
        state->active_clone_is_rescue = false;
        state->active_clone_is_shrink = false;
        update_action_state(*state);
        InvalidateRect(window, nullptr, TRUE);
        MessageBoxW(
            window,
            L"クローン処理は停止しましたが、完了結果を画面へ通知できませんでした。\n\n"
            L"コピー先がオフラインかWindowsの「ディスクの管理」で確認し、"
            L"data\\logsの最新ログを確認してください。",
            L"クローン結果の通知に失敗しました",
            MB_OK | MB_ICONERROR);
        start_inventory(*state);
        return 0;
      }
      if (state != nullptr && wparam == kUiRefreshTimerId &&
          state->media_creation_running.load()) {
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_CLOSE:
      if (state != nullptr && state->clone_running.load()) {
        MessageBoxW(
            window,
            L"クローン実行中はアプリを終了できません。\n取り消す場合は、画面右下の「安全に取り消す」を押してください。",
            L"ドライブをクローン中です",
            MB_OK | MB_ICONWARNING);
        return 0;
      }
      if (state != nullptr && state->backup_running.load()) {
        MessageBoxW(
            window,
            L"イメージ作成中はアプリを終了できません。\n取り消す場合は、画面右下の「安全に取り消す」を押してください。",
            L"イメージを作成中です",
            MB_OK | MB_ICONWARNING);
        return 0;
      }
      if (state != nullptr && state->restore_running.load()) {
        MessageBoxW(
            window,
            L"イメージ復元中はアプリを終了できません。\n取り消す場合は、画面右下の「安全に取り消す」を押してください。",
            L"イメージを復元中です",
            MB_OK | MB_ICONWARNING);
        return 0;
      }
      if (state != nullptr &&
          state->media_creation_running.load()) {
        MessageBoxW(
            window,
            L"WinPEイメージまたはISOを構成中です。"
            L"\n安全に完了するまでこのアプリを終了できません。",
            L"レスキューISOを作成中です",
            MB_OK | MB_ICONWARNING);
        return 0;
      }
      if (state != nullptr &&
          (state->support_zip_planning.load() ||
           state->support_zip_creation_running.load())) {
        MessageBoxW(
            window,
            L"サポートZIPの準備または作成中です。安全に完了するまで、このアプリを終了できません。",
            L"サポートZIPを作成中です",
            MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (state != nullptr && state->adk_management_running.load()) {
        MessageBoxW(
            window,
            L"ADK取得・管理の確認中です。結果が確定するまで、このアプリを終了できません。",
            L"ADK取得・管理を確認中です",
            MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (state != nullptr) {
        state->media_usb_inspection_cancel_requested.store(true);
        state->clean_close_requested = true;
      }
      break;
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT: {
      if (state == nullptr) {
        break;
      }
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      HDC buffer = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(
          dc, client.right, client.bottom);
      const auto previous =
          static_cast<HBITMAP>(SelectObject(buffer, bitmap));
      paint_window(*state, buffer);
      BitBlt(
          dc,
          0,
          0,
          client.right,
          client.bottom,
          buffer,
          0,
          0,
          SRCCOPY);
      SelectObject(buffer, previous);
      DeleteObject(bitmap);
      DeleteDC(buffer);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      if (state != nullptr) {
        const bool active_write_operation =
            state->clone_running.load() ||
            state->backup_running.load() ||
            state->restore_running.load() ||
            state->media_creation_running.load() ||
            state->media_preflight_running.load() ||
            state->restore_preflight_running.load() ||
            state->manual_update_running.load() ||
            state->adk_management_running.load() ||
            state->support_zip_planning.load() ||
            state->support_zip_creation_running.load();
        state->clone_cancel_requested.store(true);
        state->backup_cancel_requested.store(true);
        state->restore_preflight_cancel_requested.store(true);
        state->restore_cancel_requested.store(true);
        state->media_creation_cancel_requested.store(true);
        state->media_usb_inspection_cancel_requested.store(true);
        if (state->clone_pause_controller != nullptr) {
          static_cast<void>(
              state->clone_pause_controller->request_cancel());
        }
        if (state->backup_pause_controller != nullptr) {
          static_cast<void>(
              state->backup_pause_controller->request_cancel());
        }
        if (state->restore_pause_controller != nullptr) {
          static_cast<void>(
              state->restore_pause_controller->request_cancel());
        }
        KillTimer(window, kUiRefreshTimerId);
        KillTimer(window, kCloneCompletionFallbackTimerId);
        if (state->inventory_thread.joinable()) {
          state->inventory_thread.join();
        }
        if (state->clone_thread.joinable()) {
          state->clone_thread.join();
        }
        if (state->backup_thread.joinable()) {
          state->backup_thread.join();
        }
        if (state->media_preflight_thread.joinable()) {
          state->media_preflight_thread.join();
        }
        if (state->media_creation_thread.joinable()) {
          state->media_creation_thread.join();
        }
        if (state->media_usb_inspection_thread.joinable()) {
          state->media_usb_inspection_thread.join();
        }
        if (state->restore_preflight_thread.joinable()) {
          state->restore_preflight_thread.join();
        }
        if (state->restore_thread.joinable()) {
          state->restore_thread.join();
        }
        if (state->manual_update_thread.joinable()) {
          state->manual_update_thread.join();
        }
        if (state->support_zip_thread.joinable()) {
          state->support_zip_thread.join();
        }
        if (state->adk_management_thread.joinable()) {
          state->adk_management_thread.join();
        }
        if (state->logger.has_value()) {
          state->logger->info(L"Y-TEC Tsumugi Drive 終了");
        }
        complete_product_log_on_shutdown(
            *state, active_write_operation);
        DeleteObject(state->body_font);
        DeleteObject(state->small_font);
        DeleteObject(state->heading_font);
        DeleteObject(state->brand_font);
      }
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(
    _In_ const HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ const int show_command) {
  const UiComApartment com_apartment;
  if (!com_apartment.initialized()) {
    const std::wstring error =
        L"アプリケーションのCOMを初期化できませんでした。\nWindows error: " +
        std::to_wstring(
            static_cast<std::uint32_t>(com_apartment.result()));
    MessageBoxW(
        nullptr,
        error.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }
  const auto com_security =
      ytec::vssrequester::initialize_vss_process_security();
  if (!com_security) {
    show_product_error(
        nullptr,
        kWindowTitle,
        com_security.error());
    return 1;
  }

  // The GDI layout uses logical pixels; scale the complete surface uniformly.
  SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
  INITCOMMONCONTROLSEX common_controls{
      .dwSize = sizeof(INITCOMMONCONTROLSEX),
      .dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&common_controls);

  WNDCLASSEXW window_class{
      .cbSize = sizeof(WNDCLASSEXW),
      .style = CS_HREDRAW | CS_VREDRAW,
      .lpfnWndProc = window_proc,
      .hInstance = instance,
      .hIcon = LoadIconW(nullptr, IDI_APPLICATION),
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .hbrBackground =
          static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)),
      .lpszClassName = kWindowClass,
      .hIconSm = LoadIconW(nullptr, IDI_APPLICATION)};
  if (RegisterClassExW(&window_class) == 0) {
    MessageBoxW(
        nullptr,
        L"アプリケーション画面を初期化できませんでした。",
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }

  AppState state;
#if defined(YTEC_UI_ACCEPTANCE_BUILD)
  constexpr int initial_width = 1024;
  constexpr int initial_height = 600;
#else
  constexpr int initial_width = 1280;
  constexpr int initial_height = 720;
#endif
  const HWND window = CreateWindowExW(
      0,
      kWindowClass,
      kWindowTitle,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      initial_width,
      initial_height,
      nullptr,
      nullptr,
      instance,
      &state);
  if (window == nullptr) {
    MessageBoxW(
        nullptr,
        L"メイン画面を作成できませんでした。",
        kWindowTitle,
        MB_OK | MB_ICONERROR);
    return 1;
  }

  constexpr BOOL enable_dark_title = TRUE;
  static_cast<void>(DwmSetWindowAttribute(
      window,
      20,
      &enable_dark_title,
      sizeof(enable_dark_title)));
  const bool desired_exceeds_display =
      initial_width > GetSystemMetrics(SM_CXSCREEN) ||
      initial_height > GetSystemMetrics(SM_CYSCREEN);
  ShowWindow(
      window,
      desired_exceeds_display ? SW_MAXIMIZE : show_command);
  UpdateWindow(window);
  show_first_run_guidance_if_needed(state);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    const HWND focus = GetFocus();
    const int focused_identifier =
        focus == nullptr ? 0 : GetDlgCtrlID(focus);
    const bool navigation_focused =
        focused_identifier >= kNavFirstId &&
        focused_identifier <
            kNavFirstId + static_cast<int>(state.navigation.size());
    const bool combo_box_dropped = focus != nullptr &&
        SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0) != 0;
    if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
        navigation_focused) {
      SendMessageW(focus, BM_CLICK, 0, 0);
      continue;
    }
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
        !combo_box_dropped) {
      SendMessageW(window, WM_CLOSE, 0, 0);
      continue;
    }
    if (IsDialogMessageW(window, &message) == FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}

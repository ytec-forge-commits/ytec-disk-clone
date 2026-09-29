#include "ytec/bootrepair/automatic_repair_windows.h"
#include "ytec/bootrepair/nvram_repair.h"
#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/system_partition_creation.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/bootrepair/winre_registration.h"
#include "ytec/clonecore/completion_power_action.h"
#include "ytec/clonecore/log.h"
#include "ytec/clonecore/manual_pause.h"
#include "ytec/clonecore/operation_power.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_stream.h"
#include "ytec/operationcore/windows_resume_slot_platform.h"
#include "ytec/uisupport/error_presentation.h"
#include "ytec/uisupport/private_fonts.h"
#include "ytec/winpeapp/app_runner.h"
#include "ytec/winpeapp/active_rescue_media.h"
#include "ytec/winpeapp/automatic_boot_repair_ui.h"
#include "ytec/winpeapp/completion_power_ui.h"
#include "ytec/winpeapp/dashboard.h"
#include "ytec/winpeapp/direct_image_create.h"
#include "ytec/winpeapp/direct_image_create_resume.h"
#include "ytec/winpeapp/direct_image_restore.h"
#include "ytec/winpeapp/direct_shrink_image_restore.h"
#include "ytec/winpeapp/direct_image_restore_resume.h"
#include "ytec/winpeapp/direct_image_restore_resume_backend.h"
#include "ytec/winpeapp/direct_image_restore_resume_storage.h"
#include "ytec/winpeapp/image_create_ui.h"
#include "ytec/winpeapp/image_restore_ui.h"
#include "ytec/winpeapp/offline_ntfs_direct_shrink_product.h"
#include "ytec/winpeapp/repair_layout.h"
#include "ytec/winpeapp/resume_slot_admission.h"
#include "ytec/winpeapp/rescue_clone.h"
#include "ytec/winpeapp/tsumugi_password_ui.h"
#include "ytec/winpeapp/winre_diagnostic_view.h"

#include "resource.h"

#include <Windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef YTEC_WINPE_PRODUCT_BOUNDARY
#error WinPE GUI must be built with the product safety boundary enabled.
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"YtecTsumugiDriveWinPeWindow";
// WinPE's non-client title bar cannot reliably use the bundled Japanese font.
// Keep the OS-managed caption ASCII-only; Japanese remains available inside
// the client area where the bundled LINE Seed JP font is applied.
constexpr wchar_t kWindowTitle[] = L"Y-TEC Tsumugi Drive - WinPE";
static_assert([] {
  for (const auto character : kWindowTitle) {
    if (character > 0x7f) {
      return false;
    }
  }
  return true;
}(), "The WinPE non-client caption must remain ASCII-only.");
constexpr wchar_t kAutoSystemPartitionLabel[] = L"自動：未割当領域";
constexpr int kClonePathLabelTop = 179;
constexpr int kClonePathLabelBottom = 200;
constexpr int kClonePathControlTop = 210;
static_assert(
    kClonePathControlTop - kClonePathLabelBottom >= 8,
    "The WinPE clone label and path controls need a visible vertical gap.");
constexpr int kSidebarConceptTop = 134;
constexpr int kSidebarConceptBottom = 160;
constexpr int kSidebarNavTop = 176;
constexpr int kSidebarNavHeight = 44;
constexpr int kSidebarNavPitch = 52;
constexpr int kSidebarModeTop = 452;
constexpr int kRepairConfirmTitleTop = 328;
constexpr int kRepairConfirmTitleBottom = 350;
constexpr int kRepairAcknowledgeTop = 358;
constexpr int kRepairAcknowledgeHeight = 24;
constexpr int kRepairTokenLabelTop = 390;
constexpr int kRepairTokenLabelBottom = 411;
constexpr int kRepairTokenControlTop = 419;
constexpr int kRepairTokenControlHeight = 30;
constexpr int kRepairResultLabelTop = 461;
constexpr int kRepairResultLabelBottom = 484;
constexpr int kRepairOutputTop = 490;
static_assert(
    kSidebarNavTop - kSidebarConceptBottom >= 12,
    "The sidebar concept line and first navigation button need a visible gap.");
static_assert(
    kSidebarModeTop -
            (kSidebarNavTop + 4 * kSidebarNavPitch + kSidebarNavHeight) >=
        12,
    "The sidebar navigation and environment label need a visible gap.");
static_assert(
    kRepairAcknowledgeTop - kRepairConfirmTitleBottom >= 8 &&
        kRepairTokenLabelTop -
                (kRepairAcknowledgeTop + kRepairAcknowledgeHeight) >=
            8 &&
        kRepairTokenControlTop - kRepairTokenLabelBottom >= 8 &&
        kRepairResultLabelTop -
                (kRepairTokenControlTop + kRepairTokenControlHeight) >=
            8 &&
        kRepairOutputTop - kRepairResultLabelBottom >= 6,
    "The WinPE repair confirmation text and controls must not overlap.");

constexpr UINT kInventoryCompleteMessage = WM_APP + 1U;
constexpr UINT kCloneCheckCompleteMessage = WM_APP + 2U;
constexpr UINT kBootInspectCompleteMessage = WM_APP + 3U;
constexpr UINT kBootExecuteCompleteMessage = WM_APP + 4U;
constexpr UINT kCloneProgressMessage = WM_APP + 5U;
constexpr UINT kCloneExecuteCompleteMessage = WM_APP + 6U;
constexpr UINT kWinReDiagnosticCompleteMessage = WM_APP + 7U;
constexpr UINT kImageCreateProgressMessage = WM_APP + 8U;
constexpr UINT kImageCreateCompleteMessage = WM_APP + 9U;
constexpr UINT kImageRestoreVerifyCompleteMessage = WM_APP + 10U;
constexpr UINT kImageRestoreProgressMessage = WM_APP + 11U;
constexpr UINT kImageRestoreCompleteMessage = WM_APP + 12U;
constexpr UINT kManualPauseStateChangedMessage = WM_APP + 13U;
constexpr UINT kCloneShrinkInspectionCompleteMessage = WM_APP + 14U;

constexpr int kNavCloneId = 100;
constexpr int kNavRepairId = 101;
constexpr int kNavDiskId = 102;
constexpr int kNavImageCreateId = 103;
constexpr int kNavImageRestoreId = 104;
constexpr int kRefreshId = 200;
constexpr int kClonePathId = 201;
constexpr int kCloneBrowseId = 202;
constexpr int kCloneCheckId = 203;
constexpr int kCloneOutputId = 204;
constexpr int kCloneAcknowledgeId = 205;
constexpr int kCloneTokenId = 206;
constexpr int kCloneExecuteId = 207;
constexpr int kCloneCancelId = 208;
constexpr int kCloneRescueModeId = 209;
constexpr int kDiskListId = 210;
constexpr int kDiskDetailsId = 211;
constexpr int kRepairDiskId = 220;
constexpr int kWindowsRootId = 221;
constexpr int kSystemRootId = 222;
constexpr int kFirmwareId = 223;
constexpr int kRepairInspectId = 224;
constexpr int kRepairAcknowledgeId = 225;
constexpr int kRepairTokenId = 226;
constexpr int kRepairExecuteId = 227;
constexpr int kRepairOutputId = 228;
constexpr int kWinReDiagnosticId = 229;
constexpr int kRepairCancelId = 230;
constexpr int kClonePauseId = 231;
constexpr int kImageVerificationModeId = 238;
constexpr int kImageRescueModeId = 239;
constexpr int kImageSourceId = 240;
constexpr int kImageDestinationId = 241;
constexpr int kImageBrowseId = 242;
constexpr int kImageReviewId = 243;
constexpr int kImageTokenId = 244;
constexpr int kImageExecuteId = 245;
constexpr int kImageCancelId = 246;
constexpr int kImageOutputId = 247;
constexpr int kImageEncryptionId = 248;
constexpr int kImagePauseId = 249;
constexpr int kImageRestorePathId = 250;
constexpr int kImageRestoreBrowseId = 251;
constexpr int kImageRestoreVerifyId = 252;
constexpr int kImageRestoreTargetId = 253;
constexpr int kImageRestoreReviewId = 254;
constexpr int kImageRestoreTokenId = 255;
constexpr int kImageRestoreExecuteId = 256;
constexpr int kImageRestoreCancelId = 257;
constexpr int kImageRestoreOutputId = 258;
constexpr int kImageRestorePauseId = 259;
constexpr int kImageRestoreSourcePartitionId = 260;
constexpr int kImageRestoreTargetPartitionId = 261;
constexpr int kDirectShrinkPartitionListId = 270;
constexpr int kDirectShrinkSurplusPolicyId = 271;
constexpr int kDirectShrinkSurplusTargetId = 272;
constexpr int kDirectShrinkStatusId = 273;
constexpr int kCompletionPowerNoneId = 3401;
constexpr int kCompletionPowerRestartId = 3403;
constexpr int kCompletionPowerShutdownId = 3404;
constexpr int kErrorCopyDetailsId = 42001;
constexpr std::size_t kMaximumErrorDialogDetailCharacters = 240U;
static_assert(
    kMaximumErrorDialogDetailCharacters <=
        ytec::uisupport::kMaximumErrorDetailCharacters,
    "The expanded error preview must remain inside the shared detail bound.");

constexpr COLORREF kCanvas = RGB(244, 247, 249);
constexpr COLORREF kSidebar = RGB(29, 37, 49);
constexpr COLORREF kSidebarSelected = RGB(51, 64, 81);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kInk = RGB(29, 40, 52);
constexpr COLORREF kMuted = RGB(91, 105, 118);
constexpr COLORREF kBorder = RGB(214, 222, 228);
constexpr COLORREF kTsumugiBlue = RGB(30, 145, 160);
constexpr COLORREF kTsumugiPurple = RGB(121, 91, 174);
constexpr COLORREF kSafeGreen = RGB(42, 137, 93);
constexpr COLORREF kWarning = RGB(183, 112, 26);
constexpr COLORREF kDanger = RGB(179, 59, 59);

class ThreadSleepPrevention final {
 public:
  ThreadSleepPrevention() noexcept
      : active_(SetThreadExecutionState(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0U) {}

  ~ThreadSleepPrevention() {
    static_cast<void>(release());
  }

  ThreadSleepPrevention(const ThreadSleepPrevention&) = delete;
  ThreadSleepPrevention& operator=(const ThreadSleepPrevention&) = delete;

  [[nodiscard]] bool active() const noexcept { return active_; }

  [[nodiscard]] ytec::clonecore::SleepPreventionReleaseState
  release() noexcept {
    if (!active_) {
      return release_state_;
    }
    if (SetThreadExecutionState(ES_CONTINUOUS) == 0U) {
      release_state_ = ytec::clonecore::
          SleepPreventionReleaseState::release_failed;
      return release_state_;
    }
    active_ = false;
    release_state_ =
        ytec::clonecore::SleepPreventionReleaseState::released;
    return release_state_;
  }

 private:
  bool active_{};
  ytec::clonecore::SleepPreventionReleaseState release_state_{
      ytec::clonecore::SleepPreventionReleaseState::unknown};
};

bool confirm_long_operation_power(
    const HWND parent,
    const std::wstring_view operation) {
  const auto observation =
      ytec::clonecore::query_windows_power_observation();
  const auto advisory =
      ytec::clonecore::evaluate_long_operation_power(observation);
  if (!advisory.additional_confirmation_required) {
    return true;
  }
  const std::wstring message =
      std::wstring(operation) + L"は長時間かかる場合があります。\n\n" +
      advisory.message +
      L"\n\nこの電源状態のまま開始しますか？";
  return MessageBoxW(
             parent,
             message.c_str(),
             L"電源状態の追加確認",
             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

enum class Page : std::uint8_t {
  clone,
  image_create,
  image_restore,
  boot_repair,
  disk_diagnostics,
};

enum class CloneRoute : std::uint8_t {
  exact,
  shrink,
  rescue,
  mbr_to_gpt,
};

struct InventoryPayload final {
  std::optional<ytec::diskmodel::InventoryReport> inventory;
  std::wstring error;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct CloneCheckPayload final {
  bool success{};
  bool execution_ready{};
  CloneRoute route{CloneRoute::exact};
  std::optional<ytec::winpeapp::DirectCloneOperationPlan> reviewed_plan;
  std::optional<ytec::winpeapp::RescueCloneOperationPlan>
      reviewed_rescue_plan;
  std::optional<ytec::winpeapp::Mbr2GptDirectOperationPlan>
      reviewed_mbr2gpt_plan;
  std::shared_ptr<const
      ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan>
      reviewed_direct_shrink_plan;
  std::wstring confirmation_token;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct CloneShrinkInspectionPayload final {
  std::optional<
      ytec::winpeapp::WinPeOfflineNtfsPartitionInspection>
      inspection;
  ytec::winpeapp::WinPeOfflineNtfsProductPlanningRequest request;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct CloneProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct CloneExecutePayload final {
  bool success{};
  CloneRoute route{CloneRoute::exact};
  ytec::winpeapp::WinPeCompletionPowerProof completion_power_proof;
  std::wstring completion_power_target;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct BootInspectPayload final {
  std::optional<ytec::bootrepair::AutomaticBootRepairPlan> plan;
  std::optional<ytec::bootrepair::ReviewedSystemPartitionCreation>
      system_partition_creation;
  std::optional<ytec::bootrepair::ReviewedAutomaticBootRepairChoices>
      choices;
  std::optional<
      ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution>
      execution;
  std::optional<ytec::bootrepair::ReviewedEfiDeletePlan>
      efi_delete_plan;
  std::vector<ytec::bootrepair::BootRepairTargetSelection> selections;
  std::wstring output;
  std::wstring error;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct BootExecutePayload final {
  bool success{};
  bool partial{};
  bool system_partition_creation_stage{};
  ytec::winpeapp::WinPeCompletionPowerProof completion_power_proof;
  std::wstring completion_power_target;
  std::optional<ytec::bootrepair::AutomaticBootRepairPlan>
      completed_creation_plan;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct WinReDiagnosticPayload final {
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct ImageCreateProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct ImageCreatePayload final {
  bool success{};
  bool persistent_resume_attempt{};
  bool resume_platform_ready{};
  bool resume_slot_present{};
  std::optional<ytec::winpeapp::DirectImageCreateResumeStartupObservation>
      resume_observation_after_operation;
  ytec::winpeapp::WinPeCompletionPowerProof completion_power_proof;
  std::wstring completion_power_target;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
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
  bool warn_if_weak{};
  std::string password;

  ~TsumugiPasswordDialogState() { erase_secret(password); }
};

struct TsumugiPasswordPromptResult final {
  bool accepted{};
  std::shared_ptr<SecureAsciiPassword> password;
};

static_assert(
    ytec::winpeapp::winpe_tsumugi_password_security_policy()
        .no_recovery_key &&
        ytec::winpeapp::winpe_tsumugi_password_security_policy()
            .secret_memory_only &&
        !ytec::winpeapp::winpe_tsumugi_password_security_policy()
             .secret_logging_allowed &&
        !ytec::winpeapp::winpe_tsumugi_password_security_policy()
             .secret_persistence_allowed &&
        ytec::winpeapp::winpe_tsumugi_password_security_policy()
            .erase_secret_on_scope_exit,
    "The WinPE password dialog must retain the memory-only/no-log policy.");

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
          L"8文字以上のASCII印字文字。保存・記録しません。回復キーはありません。");
    }
    if (state->font != nullptr) {
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

      std::string confirmation;
      const bool confirmation_valid = !state->require_confirmation ||
          read_ascii_password_control(
              dialog, IDC_PASSWORD_CONFIRM_EDIT, confirmation);
      const auto create_decision = state->require_confirmation
          ? ytec::winpeapp::decide_winpe_tsumugi_create_password(
                true, password, confirmation, false)
          : ytec::winpeapp::WinPeTsumugiCreatePasswordDecision::
                password_ready;
      const auto restore_evaluation = state->require_confirmation
          ? ytec::winpeapp::WinPeTsumugiPasswordEvaluation{}
          : ytec::winpeapp::evaluate_winpe_tsumugi_restore_password(
                true, password);

      if (!confirmation_valid ||
          (state->require_confirmation &&
           create_decision ==
               ytec::winpeapp::WinPeTsumugiCreatePasswordDecision::
                   rejected) ||
          (!state->require_confirmation && !restore_evaluation.accepted)) {
        const bool mismatch = state->require_confirmation &&
            confirmation_valid &&
            !ytec::winpeapp::evaluate_winpe_tsumugi_create_password(
                 true, password, confirmation)
                 .confirmation_matches;
        erase_secret(confirmation);
        erase_secret(password);
        MessageBoxW(
            dialog,
            mismatch
                ? L"確認用パスワードが一致しません。"
                : L"8文字以上のASCII印字文字で入力してください。",
            L"パスワードを確認してください",
            MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      erase_secret(confirmation);

      if (state->warn_if_weak &&
          create_decision ==
              ytec::winpeapp::WinPeTsumugiCreatePasswordDecision::
                  weak_warning_required) {
        const bool accepted = MessageBoxW(
            dialog,
            L"このパスワードは短いか、文字種が少ないため推測されやすい可能性があります。\n\n"
            L"回復キーはなく、紛失した場合は復元できません。弱いパスワードのまま続けますか？",
            L"弱いパスワードの確認",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        if (ytec::winpeapp::decide_winpe_tsumugi_create_password(
                true, password, password, accepted) !=
            ytec::winpeapp::WinPeTsumugiCreatePasswordDecision::
                password_ready) {
          erase_secret(password);
          return TRUE;
        }
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
    const bool warn_if_weak,
    std::wstring confirm_button_label) {
  TsumugiPasswordDialogState state{
      .prompt = std::move(prompt),
      .title = std::move(title),
      .confirm_button_label = std::move(confirm_button_label),
      .font = font,
      .require_confirmation = require_confirmation,
      .warn_if_weak = warn_if_weak,
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
  if (state.password.empty()) {
    return {};
  }
  return TsumugiPasswordPromptResult{
      .accepted = true,
      .password = std::make_shared<SecureAsciiPassword>(state.password),
  };
}

struct DirectShrinkPartitionDialogResult final {
  std::vector<std::uint32_t> selected_source_table_indexes;
  ytec::migrationcore::ShrinkSurplusAllocation surplus_allocation{
      ytec::migrationcore::ShrinkSurplusAllocation::automatic_proportional};
  std::optional<std::uint32_t> surplus_target_source_table_index;
};

struct DirectShrinkPartitionDialogState final {
  const ytec::winpeapp::WinPeOfflineNtfsPartitionInspection* inspection{};
  HFONT font{};
  int client_width{880};
  int client_height{500};
  HWND partition_list{};
  HWND surplus_policy{};
  HWND surplus_target{};
  HWND status{};
  std::optional<DirectShrinkPartitionDialogResult> result;
};

std::wstring_view direct_shrink_partition_role_text(
    const ytec::migrationcore::MigrationPartitionRole role) noexcept {
  switch (role) {
    case ytec::migrationcore::MigrationPartitionRole::efi_system:
      return L"ESP";
    case ytec::migrationcore::MigrationPartitionRole::microsoft_reserved:
      return L"MSR";
    case ytec::migrationcore::MigrationPartitionRole::bios_system:
      return L"BIOS起動";
    case ytec::migrationcore::MigrationPartitionRole::windows:
      return L"Windows";
    case ytec::migrationcore::MigrationPartitionRole::recovery:
      return L"回復";
    case ytec::migrationcore::MigrationPartitionRole::data:
      return L"データ";
  }
  return L"不明";
}

std::wstring_view direct_shrink_file_system_text(
    const ytec::migrationcore::MigrationFileSystem file_system) noexcept {
  switch (file_system) {
    case ytec::migrationcore::MigrationFileSystem::none:
      return L"なし";
    case ytec::migrationcore::MigrationFileSystem::ntfs:
      return L"NTFS";
    case ytec::migrationcore::MigrationFileSystem::fat32:
      return L"FAT32";
    case ytec::migrationcore::MigrationFileSystem::exfat:
      return L"exFAT";
    case ytec::migrationcore::MigrationFileSystem::unsupported:
      return L"未対応（RAW）";
  }
  return L"不明";
}

std::optional<std::uint32_t> direct_shrink_list_source_index(
    const HWND list,
    const int item_index) {
  LVITEMW item{
      .mask = LVIF_PARAM,
      .iItem = item_index,
  };
  if (ListView_GetItem(list, &item) == FALSE || item.lParam <= 0 ||
      static_cast<std::uint64_t>(item.lParam) >
          (std::numeric_limits<std::uint32_t>::max)()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(item.lParam);
}

const ytec::winpeapp::WinPeOfflineNtfsPartitionCandidate*
find_direct_shrink_candidate(
    const DirectShrinkPartitionDialogState& state,
    const std::uint32_t source_table_index) {
  if (state.inspection == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(
      state.inspection->candidates.begin(),
      state.inspection->candidates.end(),
      [source_table_index](const auto& candidate) {
        return candidate.source_table_index == source_table_index;
      });
  return found == state.inspection->candidates.end() ? nullptr : &*found;
}

std::optional<ytec::migrationcore::ShrinkSurplusAllocation>
selected_direct_shrink_surplus_policy(
    const DirectShrinkPartitionDialogState& state) {
  const LRESULT selection =
      SendMessageW(state.surplus_policy, CB_GETCURSEL, 0, 0);
  if (selection == CB_ERR) {
    return std::nullopt;
  }
  const LRESULT data = SendMessageW(
      state.surplus_policy,
      CB_GETITEMDATA,
      static_cast<WPARAM>(selection),
      0);
  if (data == CB_ERR) {
    return std::nullopt;
  }
  const auto allocation =
      static_cast<ytec::migrationcore::ShrinkSurplusAllocation>(data);
  switch (allocation) {
    case ytec::migrationcore::ShrinkSurplusAllocation::automatic_proportional:
    case ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated:
    case ytec::migrationcore::ShrinkSurplusAllocation::selected_data_partition:
      return allocation;
  }
  return std::nullopt;
}

void set_direct_shrink_dialog_status(
    DirectShrinkPartitionDialogState& state,
    const std::wstring_view text) {
  if (state.status != nullptr) {
    SetWindowTextW(state.status, std::wstring(text).c_str());
  }
}

bool rebuild_direct_shrink_surplus_targets(
    DirectShrinkPartitionDialogState& state) {
  if (state.partition_list == nullptr || state.surplus_target == nullptr) {
    return false;
  }
  std::optional<std::uint32_t> previous;
  const LRESULT old_selection =
      SendMessageW(state.surplus_target, CB_GETCURSEL, 0, 0);
  if (old_selection != CB_ERR) {
    const LRESULT old_data = SendMessageW(
        state.surplus_target,
        CB_GETITEMDATA,
        static_cast<WPARAM>(old_selection),
        0);
    if (old_data != CB_ERR && old_data > 0 &&
        static_cast<std::uint64_t>(old_data) <=
            (std::numeric_limits<std::uint32_t>::max)()) {
      previous = static_cast<std::uint32_t>(old_data);
    }
  }
  if (SendMessageW(state.surplus_target, CB_RESETCONTENT, 0, 0) == CB_ERR) {
    return false;
  }
  LRESULT selected_item = CB_ERR;
  const int item_count = ListView_GetItemCount(state.partition_list);
  for (int index = 0; index < item_count; ++index) {
    const auto source_table_index =
        direct_shrink_list_source_index(state.partition_list, index);
    const auto* candidate = source_table_index.has_value()
        ? find_direct_shrink_candidate(state, *source_table_index)
        : nullptr;
    if (candidate == nullptr ||
        candidate->role != ytec::migrationcore::MigrationPartitionRole::data ||
        candidate->file_system !=
            ytec::migrationcore::MigrationFileSystem::ntfs ||
        ListView_GetCheckState(state.partition_list, index) == FALSE) {
      continue;
    }
    std::wstring label = L"#" + std::to_wstring(*source_table_index) + L"  ";
    label += candidate->label.empty() ? L"データ領域" : candidate->label;
    label += L"  (" + ytec::winpeapp::format_dashboard_capacity(
        candidate->source_size_bytes) + L")";
    const LRESULT item = SendMessageW(
        state.surplus_target,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    if (item == CB_ERR || item == CB_ERRSPACE ||
        SendMessageW(
            state.surplus_target,
            CB_SETITEMDATA,
            static_cast<WPARAM>(item),
            static_cast<LPARAM>(*source_table_index)) == CB_ERR) {
      return false;
    }
    if (selected_item == CB_ERR || previous == source_table_index) {
      selected_item = item;
    }
  }
  if (selected_item != CB_ERR) {
    static_cast<void>(SendMessageW(
        state.surplus_target,
        CB_SETCURSEL,
        static_cast<WPARAM>(selected_item),
        0));
  }
  const bool selected_target = selected_direct_shrink_surplus_policy(state) ==
      ytec::migrationcore::ShrinkSurplusAllocation::selected_data_partition;
  EnableWindow(state.surplus_target, selected_target ? TRUE : FALSE);
  set_direct_shrink_dialog_status(
      state,
      selected_target && selected_item == CB_ERR
          ? L"指定配分には、選択済みのNTFSデータ領域が必要です。"
          : L"Windows・起動・必須回復領域は解除できません。確定後に同じ対象を再解析します。");
  return true;
}

bool collect_direct_shrink_partition_dialog_result(
    DirectShrinkPartitionDialogState& state) {
  DirectShrinkPartitionDialogResult result;
  const int item_count = ListView_GetItemCount(state.partition_list);
  for (int index = 0; index < item_count; ++index) {
    const auto source_table_index =
        direct_shrink_list_source_index(state.partition_list, index);
    const auto* candidate = source_table_index.has_value()
        ? find_direct_shrink_candidate(state, *source_table_index)
        : nullptr;
    if (candidate == nullptr) {
      return false;
    }
    const bool selected =
        ListView_GetCheckState(state.partition_list, index) != FALSE;
    if (candidate->required && !selected) {
      set_direct_shrink_dialog_status(
          state, L"Windows・起動・必須回復領域は解除できません。");
      return false;
    }
    if (selected) {
      result.selected_source_table_indexes.push_back(*source_table_index);
    }
  }
  if (result.selected_source_table_indexes.empty()) {
    set_direct_shrink_dialog_status(
        state, L"少なくとも1つのパーティションを選択してください。");
    return false;
  }
  const auto allocation = selected_direct_shrink_surplus_policy(state);
  if (!allocation.has_value()) {
    return false;
  }
  result.surplus_allocation = *allocation;
  if (*allocation ==
      ytec::migrationcore::ShrinkSurplusAllocation::selected_data_partition) {
    const LRESULT selection =
        SendMessageW(state.surplus_target, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
      set_direct_shrink_dialog_status(
          state, L"余剰容量を受け取るNTFSデータ領域を選択してください。");
      return false;
    }
    const LRESULT data = SendMessageW(
        state.surplus_target,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selection),
        0);
    if (data == CB_ERR || data <= 0 ||
        static_cast<std::uint64_t>(data) >
            (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }
    result.surplus_target_source_table_index =
        static_cast<std::uint32_t>(data);
  }
  state.result = std::move(result);
  return true;
}

INT_PTR CALLBACK direct_shrink_partition_dialog_proc(
    const HWND dialog,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<DirectShrinkPartitionDialogState*>(
      GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<DirectShrinkPartitionDialogState*>(lparam);
    if (state == nullptr || state->inspection == nullptr) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    RECT bounds{0, 0, state->client_width, state->client_height};
    static_cast<void>(AdjustWindowRectEx(
        &bounds,
        static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
        FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE))));
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    const HMONITOR display = MonitorFromWindow(
        GetWindow(dialog, GW_OWNER), MONITOR_DEFAULTTONEAREST);
    if (display != nullptr && GetMonitorInfoW(display, &monitor) != FALSE) {
      work = monitor.rcWork;
    }
    SetWindowPos(
        dialog,
        nullptr,
        work.left + ((work.right - work.left) - width) / 2,
        work.top + ((work.bottom - work.top) - height) / 2,
        width,
        height,
        SWP_NOZORDER | SWP_NOACTIVATE);

    const auto make_control = [dialog, state](
                                  const wchar_t* class_name,
                                  const wchar_t* text,
                                  const DWORD style,
                                  const DWORD extended_style,
                                  const int identifier,
                                  const int left,
                                  const int top,
                                  const int width,
                                  const int height) {
      const HWND control = CreateWindowExW(
          extended_style,
          class_name,
          text,
          WS_CHILD | WS_VISIBLE | style,
          left,
          top,
          width,
          height,
          dialog,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
          GetModuleHandleW(nullptr),
          nullptr);
      if (control != nullptr && state->font != nullptr) {
        SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(state->font),
            TRUE);
      }
      return control;
    };
    const int margin = 18;
    const int list_top = 58;
    const int list_height = state->client_height - 238;
    const int combo_top = list_top + list_height + 32;
    const int combo_width = (state->client_width - margin * 2 - 150) / 2;
    const HWND guidance = make_control(
        L"STATIC",
        L"コピーする領域を選択してください。Windows起動に必要な領域は解除できません。未対応形式のデータ領域は元サイズRAWで保持し、安全に特定・検証できなければ停止します。",
        SS_LEFT,
        0,
        -1,
        margin,
        12,
        state->client_width - margin * 2,
        38);
    state->partition_list = make_control(
        WC_LISTVIEWW,
        L"",
        LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
        WS_EX_CLIENTEDGE,
        kDirectShrinkPartitionListId,
        margin,
        list_top,
        state->client_width - margin * 2,
        list_height);
    const HWND policy_label = make_control(
        L"STATIC", L"余剰容量", SS_LEFT | SS_CENTERIMAGE, 0, -1,
        margin, combo_top - 24, combo_width, 22);
    state->surplus_policy = make_control(
        L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0,
        kDirectShrinkSurplusPolicyId, margin, combo_top, combo_width, 120);
    const HWND target_label = make_control(
        L"STATIC", L"指定するNTFSデータ領域", SS_LEFT | SS_CENTERIMAGE, 0,
        -1, margin + combo_width + 16, combo_top - 24, combo_width, 22);
    state->surplus_target = make_control(
        L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0,
        kDirectShrinkSurplusTargetId, margin + combo_width + 16, combo_top,
        combo_width, 120);
    state->status = make_control(
        L"STATIC", L"", SS_LEFT, 0, kDirectShrinkStatusId, margin,
        combo_top + 38, state->client_width - margin * 2, 32);
    const HWND accept = make_control(
        L"BUTTON", L"この設定で再解析", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
        IDOK, state->client_width - 214, state->client_height - 38, 92, 26);
    const HWND cancel = make_control(
        L"BUTTON", L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP, 0,
        IDCANCEL, state->client_width - 114, state->client_height - 38, 96, 26);
    if (guidance == nullptr || state->partition_list == nullptr ||
        policy_label == nullptr || state->surplus_policy == nullptr ||
        target_label == nullptr || state->surplus_target == nullptr ||
        state->status == nullptr || accept == nullptr || cancel == nullptr) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }

    ListView_SetExtendedListViewStyleEx(
        state->partition_list,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    struct Column final {
      wchar_t* title;
      int width;
    };
    wchar_t number[] = L"番号";
    wchar_t role[] = L"役割";
    wchar_t file_system[] = L"形式";
    wchar_t label[] = L"ラベル";
    wchar_t size[] = L"元の容量";
    wchar_t selection[] = L"選択";
    const int list_width = state->client_width - margin * 2;
    std::array<Column, 6U> columns{
        Column{number, 60},
        Column{role, 104},
        Column{file_system, 90},
        Column{label, (std::max)(120, list_width - 570)},
        Column{size, 130},
        Column{selection, 160},
    };
    for (std::size_t column_index = 0; column_index < columns.size();
         ++column_index) {
      LVCOLUMNW column{
          .mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT,
          .fmt = column_index == 4U ? LVCFMT_RIGHT : LVCFMT_LEFT,
          .cx = columns[column_index].width,
          .pszText = columns[column_index].title,
      };
      if (ListView_InsertColumn(
              state->partition_list,
              static_cast<int>(column_index),
              &column) == -1) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
    }
    int item_index = 0;
    for (const auto& candidate : state->inspection->candidates) {
      std::wstring number_text =
          L"#" + std::to_wstring(candidate.source_table_index);
      LVITEMW item{
          .mask = LVIF_TEXT | LVIF_PARAM,
          .iItem = item_index,
          .pszText = number_text.data(),
          .lParam = static_cast<LPARAM>(candidate.source_table_index),
      };
      const int inserted = ListView_InsertItem(state->partition_list, &item);
      if (inserted < 0) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      const auto set_item = [state, inserted](
                                const int subitem,
                                const std::wstring& text) {
        LVITEMW value{
            .mask = LVIF_TEXT,
            .iItem = inserted,
            .iSubItem = subitem,
            .pszText = const_cast<wchar_t*>(text.c_str()),
        };
        return ListView_SetItem(state->partition_list, &value) != FALSE;
      };
      const std::wstring role_text{
          direct_shrink_partition_role_text(candidate.role)};
      const std::wstring file_system_text{
          direct_shrink_file_system_text(candidate.file_system)};
      const std::wstring label_text =
          candidate.label.empty() ? L"（なし）" : candidate.label;
      const std::wstring size_text =
          ytec::winpeapp::format_dashboard_capacity(
              candidate.source_size_bytes);
      const std::wstring selection_text =
          candidate.required ? L"必須・解除不可" : L"任意";
      if (!set_item(1, role_text) || !set_item(2, file_system_text) ||
          !set_item(3, label_text) || !set_item(4, size_text) ||
          !set_item(5, selection_text)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      ListView_SetCheckState(
          state->partition_list,
          inserted,
          candidate.selected_by_default ? TRUE : FALSE);
      ++item_index;
    }
    struct Policy final {
      ytec::migrationcore::ShrinkSurplusAllocation allocation;
      const wchar_t* label;
    };
    constexpr std::array<Policy, 3U> policies{
        Policy{
            ytec::migrationcore::ShrinkSurplusAllocation::automatic_proportional,
            L"自動配分（推奨）"},
        Policy{
            ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
            L"未割当のまま"},
        Policy{
            ytec::migrationcore::ShrinkSurplusAllocation::selected_data_partition,
            L"指定したNTFSデータ領域へ配分"},
    };
    for (const auto& policy : policies) {
      const LRESULT index = SendMessageW(
          state->surplus_policy,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(policy.label));
      if (index == CB_ERR || index == CB_ERRSPACE ||
          SendMessageW(
              state->surplus_policy,
              CB_SETITEMDATA,
              static_cast<WPARAM>(index),
              static_cast<LPARAM>(policy.allocation)) == CB_ERR) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
    }
    if (SendMessageW(state->surplus_policy, CB_SETCURSEL, 0, 0) == CB_ERR ||
        !rebuild_direct_shrink_surplus_targets(*state)) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    SetFocus(state->partition_list);
    return FALSE;
  }
  if (message == WM_NOTIFY && state != nullptr &&
      reinterpret_cast<const NMHDR*>(lparam)->idFrom ==
          kDirectShrinkPartitionListId) {
    const auto* header = reinterpret_cast<const NMHDR*>(lparam);
    if (header->code == LVN_ITEMCHANGING) {
      const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
      if ((change->uChanged & LVIF_STATE) != 0U) {
        const UINT old_check =
            (change->uOldState & LVIS_STATEIMAGEMASK) >> 12U;
        const UINT new_check =
            (change->uNewState & LVIS_STATEIMAGEMASK) >> 12U;
        const auto source_table_index = direct_shrink_list_source_index(
            state->partition_list, change->iItem);
        const auto* candidate = source_table_index.has_value()
            ? find_direct_shrink_candidate(*state, *source_table_index)
            : nullptr;
        if (candidate != nullptr && candidate->required && old_check == 2U &&
            new_check == 1U) {
          MessageBeep(MB_ICONWARNING);
          set_direct_shrink_dialog_status(
              *state,
              L"Windows・起動・必須回復領域は解除できません。");
          SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
          return TRUE;
        }
      }
    } else if (header->code == LVN_KEYDOWN) {
      const auto* key = reinterpret_cast<const NMLVKEYDOWN*>(lparam);
      if (key->wVKey == VK_SPACE) {
        const int focused = ListView_GetNextItem(
            state->partition_list, -1, LVNI_FOCUSED);
        const auto source_table_index = focused >= 0
            ? direct_shrink_list_source_index(state->partition_list, focused)
            : std::nullopt;
        const auto* candidate = source_table_index.has_value()
            ? find_direct_shrink_candidate(*state, *source_table_index)
            : nullptr;
        if (candidate != nullptr && candidate->required &&
            ListView_GetCheckState(state->partition_list, focused) != FALSE) {
          MessageBeep(MB_ICONWARNING);
          set_direct_shrink_dialog_status(
              *state, L"Spaceキーでも必須領域は解除できません。");
          SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
          return TRUE;
        }
      }
    } else if (header->code == LVN_ITEMCHANGED) {
      static_cast<void>(rebuild_direct_shrink_surplus_targets(*state));
    }
  }
  if (message == WM_COMMAND && state != nullptr) {
    if (LOWORD(wparam) == kDirectShrinkSurplusPolicyId &&
        HIWORD(wparam) == CBN_SELCHANGE) {
      static_cast<void>(rebuild_direct_shrink_surplus_targets(*state));
      return TRUE;
    }
    if (LOWORD(wparam) == IDOK) {
      if (collect_direct_shrink_partition_dialog_result(*state)) {
        EndDialog(dialog, IDOK);
      } else {
        MessageBeep(MB_ICONWARNING);
      }
      return TRUE;
    }
    if (LOWORD(wparam) == IDCANCEL) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  if (message == WM_CLOSE) {
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}

std::optional<DirectShrinkPartitionDialogResult>
show_direct_shrink_partition_review_dialog(
    const HWND owner,
    const HFONT font,
    const ytec::winpeapp::WinPeOfflineNtfsPartitionInspection& inspection) {
  RECT work{0, 0, 1024, 600};
  MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
  const HMONITOR display =
      MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
  if (display != nullptr && GetMonitorInfoW(display, &monitor) != FALSE) {
    work = monitor.rcWork;
  }
  DirectShrinkPartitionDialogState state{
      .inspection = &inspection,
      .font = font,
      .client_width = (std::clamp)(
          static_cast<int>(work.right - work.left) - 80, 720, 1040),
      .client_height = (std::clamp)(
          static_cast<int>(work.bottom - work.top) - 80, 480, 650),
  };
  alignas(DWORD) std::array<std::byte, 512U> storage{};
  auto* dialog_template = reinterpret_cast<DLGTEMPLATE*>(storage.data());
  dialog_template->style =
      WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
  dialog_template->dwExtendedStyle = 0;
  dialog_template->cdit = 0;
  dialog_template->x = 0;
  dialog_template->y = 0;
  dialog_template->cx = 320;
  dialog_template->cy = 240;
  WORD* words = reinterpret_cast<WORD*>(dialog_template + 1);
  *words++ = 0;
  *words++ = 0;
  constexpr std::wstring_view title{L"WinPE 縮小移行：パーティション・余剰容量"};
  for (const wchar_t character : title) {
    *words++ = static_cast<WORD>(character);
  }
  *words = 0;
  const INT_PTR result = DialogBoxIndirectParamW(
      GetModuleHandleW(nullptr),
      dialog_template,
      owner,
      direct_shrink_partition_dialog_proc,
      reinterpret_cast<LPARAM>(&state));
  if (result != IDOK || !state.result.has_value()) {
    if (result == -1) {
      MessageBoxW(
          owner,
          L"パーティション・容量設定画面を開けませんでした。",
          kWindowTitle,
          MB_OK | MB_ICONERROR);
    }
    return std::nullopt;
  }
  return std::move(state.result);
}

struct VerifiedRestoreImageSelection final {
  std::wstring path;
  ytec::imageformat::TsumugiImageStorageFileSystem storage_file_system{
      ytec::imageformat::TsumugiImageStorageFileSystem::unknown};
  std::uint32_t storage_disk_number{};
  std::optional<ytec::clonecore::StableDiskIdentity>
      stable_storage_identity;
  ytec::imageformat::Sha256Digest global_hash{};
  ytec::imageformat::Sha256Digest source_state_hash{};
  ytec::imageformat::TsumugiManifest manifest;
  ytec::imageformat::TsumugiManifestMode mode{
      ytec::imageformat::TsumugiManifestMode::exact};
  ytec::imageformat::TsumugiManifestPartitionStyle partition_style{
      ytec::imageformat::TsumugiManifestPartitionStyle::gpt};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::size_t partition_count{};
  bool partial_loss{};
  bool encrypted{};
};

struct ImageRestoreVerifyPayload final {
  std::optional<VerifiedRestoreImageSelection> selection;
  std::shared_ptr<SecureAsciiPassword> password;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct ImageRestoreProgressPayload final {
  ytec::clonecore::DiskOperationProgress progress;
  std::chrono::milliseconds elapsed{};
};

struct ImageRestorePayload final {
  bool success{};
  bool partial_loss{};
  bool boot_repair_offer_required{};
  bool shrink_restore{};
  bool persistent_resume_attempt{};
  bool resume_platform_ready{};
  bool resume_slot_present{};
  std::optional<ytec::operationcore::ResumeSlotBinding>
      reviewed_resume_slot_after_operation;
  ytec::winpeapp::WinPeCompletionPowerProof completion_power_proof;
  std::wstring completion_power_target;
  std::wstring output;
  std::optional<ytec::uisupport::ErrorPresentation> error_presentation;
};

struct AppState final {
  HWND window{};
  Page page{Page::clone};
  bool inventory_busy{};
  bool operation_busy{};
  std::uint64_t next_completion_power_operation_binding{1U};
  int clone_step{1};
  int repair_step{1};
  ytec::winpeapp::DashboardView dashboard;
  std::optional<ytec::diskmodel::InventoryReport> inventory;
  std::optional<ytec::winpeapp::DirectCloneOperationPlan>
      reviewed_clone_plan;
  std::optional<ytec::winpeapp::RescueCloneOperationPlan>
      reviewed_rescue_clone_plan;
  std::optional<ytec::winpeapp::Mbr2GptDirectOperationPlan>
      reviewed_mbr2gpt_clone_plan;
  std::shared_ptr<const
      ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan>
      reviewed_direct_shrink_clone_plan;
  std::optional<ytec::bootrepair::AutomaticBootRepairPlan>
      inspected_boot_plan;
  std::optional<ytec::bootrepair::ReviewedSystemPartitionCreation>
      inspected_system_partition_creation;
  std::optional<ytec::bootrepair::ReviewedAutomaticBootRepairChoices>
      inspected_boot_choices;
  std::optional<
      ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution>
      inspected_boot_execution;
  std::optional<ytec::bootrepair::ReviewedEfiDeletePlan>
      inspected_efi_delete_plan;
  std::vector<ytec::bootrepair::BootRepairTargetSelection>
      inspected_boot_selections;
  bool boot_execution_active{};
  bool clone_execution_ready{};
  bool clone_progress_active{};
  ytec::clonecore::DiskOperationProgress clone_progress;
  std::chrono::milliseconds clone_elapsed{};
  std::wstring clone_confirmation_token;
  std::shared_ptr<std::atomic_bool> clone_cancellation;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      clone_pause_controller;
  int image_step{1};
  std::optional<ytec::diskmodel::DiskInfo> reviewed_image_source;
  std::optional<ytec::diskmodel::ImagePartitionSelection>
      reviewed_image_partition_selection;
  std::wstring reviewed_image_path;
  bool reviewed_image_replace_existing{};
  bool reviewed_image_encrypted{};
  bool reviewed_image_rescue_mode{};
  ytec::imageformat::TsumugiCreateVerificationMode
      reviewed_image_verification_mode{
          ytec::imageformat::TsumugiCreateVerificationMode::complete};
  std::shared_ptr<SecureAsciiPassword> reviewed_image_password;
  std::optional<ytec::winpeapp::DirectImageCreateResumeStartupObservation>
      reviewed_image_resume_observation;
  bool image_resume_platform_ready{};
  bool image_resume_slot_present{};
  bool image_resume_requested{};
  bool image_execution_ready{};
  bool image_progress_active{};
  ytec::clonecore::DiskOperationProgress image_progress;
  std::chrono::milliseconds image_elapsed{};
  std::shared_ptr<std::atomic_bool> image_cancellation;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      image_pause_controller;
  int restore_step{1};
  std::optional<VerifiedRestoreImageSelection> verified_restore_image;
  std::shared_ptr<SecureAsciiPassword> reviewed_restore_image_password;
  std::optional<ytec::diskmodel::DiskInfo> reviewed_restore_target;
  std::optional<ytec::clonecore::StableDiskIdentity>
      reviewed_restore_target_identity;
  std::optional<ytec::imageformat::Sha256Digest>
      reviewed_restore_target_layout_hash;
  std::optional<ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection>
      reviewed_restore_individual_partition;
  std::optional<ytec::winpeapp::DirectShrinkImageRestoreWorkReview>
      reviewed_shrink_restore_work;
  std::optional<ytec::imageformat::
      TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
      reviewed_shrink_restore_layout;
  std::optional<ytec::clonecore::StableDiskIdentity>
      reviewed_shrink_original_source_target;
  std::optional<ytec::operationcore::ResumeSlotBinding>
      reviewed_restore_resume_slot;
  bool restore_resume_platform_ready{};
  bool restore_resume_slot_present{};
  bool restore_resume_requested{};
  std::vector<std::uint32_t> restore_source_partition_candidates;
  std::vector<ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection>
      restore_target_partition_candidates;
  bool restore_execution_ready{};
  bool restore_progress_active{};
  bool restore_write_active{};
  ytec::clonecore::DiskOperationProgress restore_progress;
  std::chrono::milliseconds restore_elapsed{};
  std::shared_ptr<std::atomic_bool> restore_cancellation;
  std::shared_ptr<ytec::clonecore::ManualPauseController>
      restore_pause_controller;
  std::wstring boot_confirmation_token;
  std::vector<std::wstring> roots;

  HWND nav_clone{};
  HWND nav_image_create{};
  HWND nav_image_restore{};
  HWND nav_repair{};
  HWND nav_disk{};
  HWND refresh{};
  HWND clone_path{};
  HWND clone_browse{};
  HWND clone_check{};
  HWND clone_rescue_mode{};
  HWND clone_acknowledge{};
  HWND clone_token{};
  HWND clone_execute{};
  HWND clone_cancel{};
  HWND clone_pause{};
  HWND clone_output{};
  HWND image_source{};
  HWND image_destination{};
  HWND image_browse{};
  HWND image_rescue_mode{};
  HWND image_verification_mode{};
  HWND image_encryption{};
  HWND image_review{};
  HWND image_token{};
  HWND image_execute{};
  HWND image_cancel{};
  HWND image_pause{};
  HWND image_output{};
  HWND restore_path{};
  HWND restore_browse{};
  HWND restore_verify{};
  HWND restore_source_partition{};
  HWND restore_target{};
  HWND restore_target_partition{};
  HWND restore_review{};
  HWND restore_token{};
  HWND restore_execute{};
  HWND restore_cancel{};
  HWND restore_pause{};
  HWND restore_output{};
  HWND disk_list{};
  HWND disk_details{};
  HWND repair_disk{};
  HWND windows_root{};
  HWND system_root{};
  HWND firmware{};
  HWND repair_inspect{};
  HWND winre_diagnostic{};
  HWND repair_acknowledge{};
  HWND repair_token{};
  HWND repair_execute{};
  HWND repair_cancel{};
  HWND repair_output{};

  HFONT title_font{};
  HFONT heading_font{};
  HFONT body_font{};
  HFONT small_font{};
  HFONT mono_font{};
  ytec::uisupport::PrivateFontCollection private_fonts;
  HBRUSH canvas_brush{};
  HBRUSH card_brush{};
};

void populate_restore_source_partition_candidates(AppState& state);
void populate_restore_target_partition_candidates(AppState& state);
std::optional<std::uint32_t> selected_restore_source_partition(
    const AppState& state);
std::optional<ytec::imageformat::
    TsumugiPhysicalIndividualPartitionRestoreSelection>
current_restore_individual_partition_selection(const AppState& state);
std::optional<ytec::imageformat::TsumugiCreateVerificationMode>
selected_image_create_verification_mode(const AppState& state);
std::wstring_view image_create_verification_mode_name(
    ytec::imageformat::TsumugiCreateVerificationMode mode) noexcept;

std::wstring control_text(HWND control);
void update_action_state(AppState& state);
std::optional<ytec::diskmodel::DiskInfo> find_inventory_disk(
    const AppState& state,
    std::uint32_t disk_number);
std::string current_utc_timestamp();

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

void update_manual_pause_button(
    const HWND button,
    const bool operation_active,
    const std::shared_ptr<ytec::clonecore::ManualPauseController>&
        controller) {
  ShowWindow(button, operation_active ? SW_SHOW : SW_HIDE);
  if (!operation_active || controller == nullptr) {
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

void handle_manual_pause_button(
    AppState& state,
    const std::shared_ptr<ytec::clonecore::ManualPauseController>&
        controller,
    const HWND output) {
  if (controller == nullptr) {
    return;
  }
  const auto before = controller->snapshot();
  if (!toggle_manual_pause(controller)) {
    return;
  }
  std::wstring text = control_text(output);
  text += before.state == ytec::clonecore::ManualPauseState::running
      ? L"\r\n\r\n一時停止要求を受け付けました。検証済みの安全な境界で停止します。"
      : L"\r\n\r\n再開要求を受け付けました。安全な処理を続行します。";
  SetWindowTextW(output, text.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
}

bool copy_error_text_to_clipboard(
    const HWND owner,
    const std::wstring_view text) noexcept {
  if (text.empty() ||
      text.size() > ytec::uisupport::kMaximumErrorCopyCharacters ||
      OpenClipboard(owner) == FALSE) {
    return false;
  }

  bool copied = false;
  HGLOBAL memory{};
  if (EmptyClipboard() != FALSE) {
    const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
    memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
      auto* buffer = static_cast<wchar_t*>(GlobalLock(memory));
      if (buffer != nullptr) {
        std::copy(text.begin(), text.end(), buffer);
        buffer[text.size()] = L'\0';
        GlobalUnlock(memory);
        if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
          copied = true;
          memory = nullptr;
        }
      }
    }
  }
  if (memory != nullptr) {
    GlobalFree(memory);
  }
  CloseClipboard();
  return copied;
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
      L"\r\n＜続きは「詳細をコピー」で確認できます＞";
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
      copy_error_text_to_clipboard(dialog, context->copy_text);
  const wchar_t* status = copied
      ? L"詳細をクリップボードへコピーしました。"
      : L"詳細をコピーできませんでした。もう一度お試しください。";
  SendMessageW(
      dialog,
      TDM_SET_ELEMENT_TEXT,
      TDE_FOOTER,
      reinterpret_cast<LPARAM>(status));
  return S_FALSE;
}

void restore_error_dialog_focus(const HWND previous_focus) noexcept {
  if (previous_focus != nullptr && IsWindow(previous_focus) != FALSE &&
      IsWindowEnabled(previous_focus) != FALSE &&
      IsWindowVisible(previous_focus) != FALSE) {
    SetFocus(previous_focus);
  }
}

void show_error_presentation(
    const HWND owner,
    const ytec::uisupport::ErrorPresentation& presentation) {
  const HWND previous_focus = GetFocus();
  const std::wstring content =
      L"コード: " + presentation.code +
      L"\r\n次の操作: " + presentation.next_action;
  const std::wstring expanded_details =
      bounded_error_detail_preview(presentation.details);
  ErrorTaskDialogContext context{
      .copy_text =
          ytec::uisupport::format_error_details_for_copy(presentation),
  };
  constexpr TASKDIALOG_BUTTON kButtons[]{
      {kErrorCopyDetailsId, L"詳細をコピー"},
  };
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_SIZE_TO_CONTENT;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = L"処理を完了できませんでした";
  config.pszMainIcon = TD_ERROR_ICON;
  config.pszMainInstruction = presentation.summary.c_str();
  config.pszContent = content.c_str();
  if (presentation.details_expandable && !expanded_details.empty()) {
    config.pszExpandedInformation = expanded_details.c_str();
    config.pszExpandedControlText = L"詳細を表示";
    config.pszCollapsedControlText = L"詳細を隠す";
  }
  if (presentation.details_copyable && !context.copy_text.empty()) {
    config.cButtons = static_cast<UINT>(std::size(kButtons));
    config.pButtons = kButtons;
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

  int pressed{};
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result)) {
    std::wstring fallback =
        ytec::uisupport::format_error_primary(presentation);
    fallback +=
        L"\r\n\r\n詳細表示を利用できません。"
        L"詳細をクリップボードへコピーしますか？";
    if (MessageBoxW(
            owner,
            fallback.c_str(),
            L"処理を完了できませんでした",
            MB_YESNO | MB_ICONERROR | MB_DEFBUTTON2) == IDYES) {
      const bool copied =
          copy_error_text_to_clipboard(owner, context.copy_text);
      MessageBoxW(
          owner,
          copied
              ? L"詳細をクリップボードへコピーしました。"
              : L"詳細をコピーできませんでした。",
          L"エラー詳細",
          MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
  }
  restore_error_dialog_focus(previous_focus);
}

std::wstring format_error(
    const ytec::clonecore::Error& error,
    std::optional<ytec::uisupport::ErrorPresentation>* const captured =
        nullptr) {
  const auto presentation =
      ytec::uisupport::make_error_presentation(error);
  if (captured != nullptr) {
    *captured = presentation;
  }
  return ytec::uisupport::format_error_primary(presentation);
}

void set_error_output_and_show(
    const HWND owner,
    const HWND output,
    const ytec::clonecore::Error& error,
    const std::wstring_view safety_suffix = {}) {
  const auto presentation =
      ytec::uisupport::make_error_presentation(error);
  std::wstring text =
      ytec::uisupport::format_error_primary(presentation);
  if (!safety_suffix.empty()) {
    text += L"\r\n\r\n";
    text += safety_suffix;
  }
  SetWindowTextW(output, text.c_str());
  show_error_presentation(owner, presentation);
}

void show_captured_error(
    const HWND owner,
    const std::optional<ytec::uisupport::ErrorPresentation>& presentation) {
  if (presentation.has_value()) {
    show_error_presentation(owner, presentation.value());
  }
}

[[nodiscard]] int completion_power_radio_id(
    const ytec::clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case ytec::clonecore::CompletionPowerAction::none:
      return kCompletionPowerNoneId;
    case ytec::clonecore::CompletionPowerAction::restart:
      return kCompletionPowerRestartId;
    case ytec::clonecore::CompletionPowerAction::shutdown:
      return kCompletionPowerShutdownId;
    case ytec::clonecore::CompletionPowerAction::sleep:
      return 0;
  }
  return 0;
}

[[nodiscard]] ytec::clonecore::CompletionPowerAction
completion_power_action_from_radio_id(const int radio_id) noexcept {
  switch (radio_id) {
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
    case ytec::clonecore::CompletionPowerAction::restart:
      return L"再起動";
    case ytec::clonecore::CompletionPowerAction::shutdown:
      return L"シャットダウン";
    case ytec::clonecore::CompletionPowerAction::sleep:
      return L"何もしない";
  }
  return L"何もしない";
}

// Returns true only after the platform accepted a restart/shutdown request.
// Merely displaying, cancelling, or failing either confirmation returns false.
[[nodiscard]] bool offer_winpe_completion_power_action(
    AppState& state,
    const ytec::winpeapp::WinPeCompletionPowerProof& proof,
    const std::wstring_view completed_operation,
    const std::wstring_view target_summary) {
  const auto prompt_plan =
      ytec::winpeapp::plan_winpe_completion_power_prompt(proof);
  if (!prompt_plan.prompt_allowed || completed_operation.empty() ||
      target_summary.empty()) {
    return false;
  }

  auto platform =
      ytec::clonecore::make_windows_completion_power_platform();
  if (platform == nullptr) {
    return false;
  }
  const auto availability =
      ytec::clonecore::query_completion_power_availability(
          ytec::clonecore::CompletionPowerEnvironment::winpe,
          *platform);
  const auto actions =
      ytec::clonecore::available_completion_power_actions(availability);
  constexpr std::array<std::wstring_view, 4U> kLabels{
      L"何もしない（既定）",
      L"スリープ（WinPEでは使用不可）",
      L"再起動",
      L"シャットダウン",
  };
  std::vector<TASKDIALOG_BUTTON> radio_buttons;
  radio_buttons.reserve(actions.size());
  for (const auto action : actions) {
    const auto index = static_cast<std::size_t>(action);
    const int radio_id = completion_power_radio_id(action);
    if (index >= kLabels.size() || radio_id == 0) {
      continue;
    }
    radio_buttons.push_back(TASKDIALOG_BUTTON{
        radio_id,
        kLabels[index].data(),
    });
  }
  if (radio_buttons.size() != 3U ||
      radio_buttons.front().nButtonID != kCompletionPowerNoneId) {
    return false;
  }

  const HWND previous_focus = GetFocus();
  const std::wstring instruction =
      std::wstring(completed_operation) + L"が安全に完了しました";
  const std::wstring content =
      L"対象: " + std::wstring(target_summary) +
      L"\r\n結果: 必須証拠の検証完了・自動スリープ防止解除済み"
      L"\r\n\r\nこのあとWinPEへ要求する動作を選んでください。";
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
  config.pszContent = content.c_str();
  config.cRadioButtons = static_cast<UINT>(radio_buttons.size());
  config.pRadioButtons = radio_buttons.data();
  config.nDefaultRadioButton = kCompletionPowerNoneId;
  config.nDefaultButton = IDOK;
  config.pszFooterIcon = TD_INFORMATION_ICON;
  config.pszFooter =
      L"WinPEではスリープを提供しません。既定は「何もしない」です。"
      L"電源操作は次の画面でもう一度確認します。";

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
      : task_dialog(&config, &pressed_button, &selected_radio, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(shown) || pressed_button != IDOK) {
    restore_error_dialog_focus(previous_focus);
    return false;
  }

  const auto selected_action =
      completion_power_action_from_radio_id(selected_radio);
  if (selected_action ==
      ytec::clonecore::kDefaultCompletionPowerAction) {
    restore_error_dialog_focus(previous_focus);
    return false;
  }

  const std::wstring reconfirmation =
      L"対象: " + std::wstring(target_summary) +
      L"\r\n結果: 必須証拠の検証完了"
      L"\r\n動作: " +
      std::wstring(completion_power_action_label(selected_action)) +
      L"\r\n\r\n対象と結果をもう一度確認しました。"
      L"この電源操作を今すぐWinPEへ要求しますか？";
  if (MessageBoxW(
          state.window,
          reconfirmation.c_str(),
          L"完了後の電源操作を再確認",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    restore_error_dialog_focus(previous_focus);
    return false;
  }

  const auto request = ytec::winpeapp::
      make_winpe_completion_power_execution_request(
          proof, selected_action, true, true);
  const auto result = ytec::clonecore::execute_completion_power_action(
      request, *platform);
  if (result.disposition == ytec::clonecore::
          CompletionPowerExecutionDisposition::request_accepted) {
    const bool session_end_expected = ytec::winpeapp::
        winpe_completion_power_action_expects_ui_session_end(
            selected_action);
    if (!session_end_expected) {
      restore_error_dialog_focus(previous_focus);
    }
    return session_end_expected;
  }
  if (result.error.has_value()) {
    show_error_presentation(
        state.window,
        ytec::uisupport::make_error_presentation(result.error.value()));
    MessageBoxW(
        state.window,
        L"元の操作と検証済み完了結果は変わりません。"
        L"このWinPEセッションはそのまま継続できます。",
        L"電源操作は実行していません",
        MB_OK | MB_ICONINFORMATION);
  }
  restore_error_dialog_focus(previous_focus);
  return false;
}

ytec::clonecore::Error restore_ui_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

ytec::clonecore::Result<std::wstring> canonical_restore_image_path(
    const std::wstring& path) {
  constexpr std::size_t kMaximumPathCharacters = 32768U;
  if (path.empty() || path.size() >= kMaximumPathCharacters ||
      path.size() < 8U ||
      _wcsicmp(path.c_str() + path.size() - 8U, L".tsumugi") != 0) {
    return ytec::clonecore::Result<std::wstring>::failure(
        restore_ui_error(
            ytec::clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"PE復元イメージ選択",
            L"既存の単一 .tsumugi ファイルだけを選択してください"));
  }
  std::vector<wchar_t> canonical(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(canonical.size()),
      canonical.data(),
      nullptr);
  if (length == 0U || length >= canonical.size()) {
    return ytec::clonecore::Result<std::wstring>::failure(
        restore_ui_error(
            ytec::clonecore::ErrorCode::invalid_argument,
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE,
            L"PE復元イメージ絶対パス",
            L"イメージの絶対パスを一意に確定できません"));
  }
  const DWORD attributes = GetFileAttributesW(canonical.data());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return ytec::clonecore::Result<std::wstring>::failure(
        restore_ui_error(
            ytec::clonecore::ErrorCode::unsupported_layout,
            attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                   : ERROR_REPARSE_TAG_INVALID,
            L"PE復元イメージ属性",
            L"存在する通常ファイルだけを使用でき、reparse pointは使用できません"));
  }
  return ytec::clonecore::Result<std::wstring>::success(
      std::wstring(canonical.data(), length));
}

ytec::clonecore::Result<
    ytec::imageformat::TsumugiImageStorageFileSystem>
query_restore_image_file_system(const std::wstring& path) {
  std::vector<wchar_t> volume_root(32768U, L'\0');
  if (!GetVolumePathNameW(
          path.c_str(),
          volume_root.data(),
          static_cast<DWORD>(volume_root.size()))) {
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiImageStorageFileSystem>::failure(
            restore_ui_error(
                ytec::clonecore::ErrorCode::query_failed,
                GetLastError(),
                L"PE復元イメージVolume確認",
                L"保存元ボリュームを確認できません"));
  }
  std::array<wchar_t, MAX_PATH + 1U> file_system{};
  if (!GetVolumeInformationW(
          volume_root.data(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiImageStorageFileSystem>::failure(
            restore_ui_error(
                ytec::clonecore::ErrorCode::query_failed,
                GetLastError(),
                L"PE復元イメージfilesystem確認",
                L"保存元filesystemを確認できません"));
  }
  if (_wcsicmp(file_system.data(), L"NTFS") == 0) {
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiImageStorageFileSystem>::success(
            ytec::imageformat::TsumugiImageStorageFileSystem::ntfs);
  }
  if (_wcsicmp(file_system.data(), L"exFAT") == 0) {
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiImageStorageFileSystem>::success(
            ytec::imageformat::TsumugiImageStorageFileSystem::exfat);
  }
  return ytec::clonecore::Result<
      ytec::imageformat::TsumugiImageStorageFileSystem>::failure(
          restore_ui_error(
              ytec::clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"PE復元イメージfilesystem",
              L"正式対応はNTFSまたはexFAT上の単一ファイルだけです"));
}

bool current_process_is_elevated() noexcept {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return false;
  }
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0U;
  const bool elevated = GetTokenInformation(
      raw_token,
      TokenElevation,
      &elevation,
      sizeof(elevation),
      &returned) != FALSE &&
      returned >= sizeof(elevation) && elevation.TokenIsElevated != 0U;
  CloseHandle(raw_token);
  return elevated;
}

bool current_environment_is_verified_winpe() noexcept {
  HKEY key = nullptr;
  const LSTATUS status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
      0,
      KEY_QUERY_VALUE,
      &key);
  if (key != nullptr) {
    RegCloseKey(key);
  }
  return status == ERROR_SUCCESS;
}

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return L"表示できる上限を超えた診断結果です。";
  }
  const int input_length = static_cast<int>(text.size());
  const int length = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      text.data(),
      input_length,
      nullptr,
      0);
  if (length <= 0) {
    return L"診断結果の文字コードを安全に解釈できません。";
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          text.data(),
          input_length,
          result.data(),
          length) != length) {
    return L"診断結果の文字コードを安全に解釈できません。";
  }
  return result;
}

std::wstring format_completion_disk_target(
    const ytec::clonecore::StableDiskIdentity& identity) {
  std::wostringstream stream;
  stream << L"ディスク " << identity.disk_number << L" / "
         << (identity.model.empty() ? L"モデル不明" : identity.model)
         << L" / "
         << ytec::winpeapp::format_dashboard_capacity(identity.size_bytes)
         << L" / シリアル末尾: "
         << (identity.serial_suffix.empty()
                 ? L"不明"
                 : utf8_to_wide(identity.serial_suffix));
  return stream.str();
}

std::wstring format_completion_disk_target(
    const ytec::diskmodel::DiskInfo& disk) {
  std::wostringstream stream;
  stream << L"ディスク " << disk.disk_number << L" / "
         << (disk.model.empty() ? L"モデル不明" : disk.model) << L" / "
         << ytec::winpeapp::format_dashboard_capacity(disk.size_bytes)
         << L" / シリアル末尾: "
         << (disk.serial_suffix.empty()
                 ? L"不明"
                 : utf8_to_wide(disk.serial_suffix));
  return stream.str();
}

[[nodiscard]] ytec::clonecore::CompletionOperationOutcome
completion_operation_outcome(
    const bool success,
    const bool partial = false) noexcept {
  if (!success) {
    return ytec::clonecore::CompletionOperationOutcome::failed;
  }
  return partial
      ? ytec::clonecore::CompletionOperationOutcome::partial
      : ytec::clonecore::CompletionOperationOutcome::succeeded;
}

[[nodiscard]] bool direct_clone_completion_verified(
    const ytec::winpeapp::CloneExecutionReport& report) noexcept {
  const bool style_known =
      report.partition_style == ytec::winpeapp::ClonePartitionStyle::gpt ||
      report.partition_style == ytec::winpeapp::ClonePartitionStyle::mbr;
  const bool boot_verified = !report.boot_finalization_required ||
      (report.boot_repair.bcdboot.microsoft_signature_verified &&
       report.boot_repair.bcdboot.exit_code == 0U &&
       report.boot_repair.boot_store_verified &&
       (!report.boot_repair.system_partition_temporarily_mounted ||
        report.boot_repair.temporary_mount_released) &&
       report.temporary_mounts_released &&
       report.boot_finalization_verified);
  return style_known && report.copied_data_bytes != 0U &&
      report.copied_partition_count != 0U && report.read_back_verified &&
      report.partition_table_committed && !report.target_returned_online &&
      report.target_left_offline && boot_verified;
}

[[nodiscard]] bool mbr2gpt_clone_completion_verified(
    const ytec::winpeapp::Mbr2GptDirectExecutionReport& report) noexcept {
  const bool intermediate_clone_verified =
      report.clone.partition_style ==
          ytec::winpeapp::ClonePartitionStyle::mbr &&
      report.clone.copied_data_bytes != 0U &&
      report.clone.copied_partition_count != 0U &&
      report.clone.read_back_verified &&
      report.clone.partition_table_committed &&
      report.clone.target_returned_online &&
      !report.clone.target_left_offline &&
      report.clone.boot_finalization_required &&
      report.clone.boot_repair.bcdboot.microsoft_signature_verified &&
      report.clone.boot_repair.bcdboot.exit_code == 0U &&
      report.clone.boot_repair.boot_store_verified &&
      report.clone.temporary_mounts_released &&
      report.clone.boot_finalization_verified;
  return intermediate_clone_verified &&
      report.conversion.microsoft_signature_verified &&
      report.conversion.target_reidentified_before_conversion &&
      report.conversion.validation.exit_code == 0U &&
      report.conversion.conversion.exit_code == 0U &&
      report.boot_repair.bcdboot.microsoft_signature_verified &&
      report.boot_repair.bcdboot.exit_code == 0U &&
      report.boot_repair.boot_store_verified &&
      (!report.boot_repair.system_partition_temporarily_mounted ||
       report.boot_repair.temporary_mount_released) &&
      report.source_reidentified_unchanged && report.source_left_read_only &&
      report.target_reidentified_as_gpt &&
      report.efi_system_partition_verified &&
      report.microsoft_reserved_partition_verified &&
      report.offline_windows_verified &&
      report.temporary_windows_mount_released &&
      report.final_layout_verified && report.final_target_left_offline;
}

[[nodiscard]] bool direct_image_create_completion_verified(
    const ytec::winpeapp::DirectImageCreateReport& report) noexcept {
  const bool common = report.imaged_partition_count != 0U &&
      report.logical_payload_bytes != 0U && report.source_read_only_verified &&
      report.source_left_read_only &&
      report.image.stream.image_length != 0U &&
      report.image.stream.chunk_count != 0U &&
      report.image.stream.committed &&
      ytec::imageformat::selected_tsumugi_creation_verification_passed(
          report.image);
  if (!common) {
    return false;
  }
  if (report.rescue_mode) {
    return report.rescue.has_value();
  }
  return report.layout_revalidated_before_commit &&
      !report.rescue.has_value() && report.image.unreadable_ranges.empty();
}

[[nodiscard]] bool direct_image_restore_completion_verified(
    const ytec::winpeapp::DirectImageRestoreReport& report) noexcept {
  const auto& physical = report.physical;
  const auto& restore = physical.restore;
  return report.active_rescue_media_checked && report.direct_execution_only &&
      physical.initial_image_verification_completed &&
      physical.target_reidentified_before_offline &&
      physical.target_handle_reidentified && physical.target_left_offline &&
      !physical.partial_loss && !restore.partial_loss &&
      restore.written_logical_bytes != 0U &&
      restore.written_chunk_count != 0U &&
      restore.callbacks_started_after_complete_verification &&
      restore.image_matched_prepared_plan &&
      restore.target_reidentified_before_write &&
      restore.all_writes_read_back_verified &&
      restore.final_layout_committed;
}

[[nodiscard]] bool direct_shrink_restore_completion_verified(
    const ytec::winpeapp::DirectShrinkImageRestoreReport& report) noexcept {
  const auto& restore = report.restore;
  const std::uint64_t first = restore.archive_logical_bytes;
  if (restore.exact_raw_logical_bytes >
      (std::numeric_limits<std::uint64_t>::max)() - first) {
    return false;
  }
  const std::uint64_t with_raw =
      first + restore.exact_raw_logical_bytes;
  if (restore.intentionally_omitted_logical_bytes >
      (std::numeric_limits<std::uint64_t>::max)() - with_raw) {
    return false;
  }
  const std::uint64_t verified_logical_bytes =
      with_raw + restore.intentionally_omitted_logical_bytes;
  return verified_logical_bytes != 0U &&
      report.image_completely_reverified &&
      report.image_backing_reidentified_before_write &&
      report.active_rescue_media_checked &&
      report.target_reidentified_before_plan &&
      report.work_placement_reidentified_before_write &&
      report.target_left_offline && report.direct_execution_only &&
      restore.callbacks_started_after_complete_verification &&
      restore.image_matched_prepared_plan &&
      restore.target_reidentified_before_write &&
      restore.all_payloads_verified_by_adapter &&
      restore.final_layout_committed;
}

[[nodiscard]] bool persistent_restore_completion_verified(
    const ytec::winpeapp::DirectImageRestoreResumeOutcome& outcome) noexcept {
  const bool completed = outcome.kind == ytec::winpeapp::
          DirectImageRestoreResumeOutcomeKind::completed ||
      outcome.kind == ytec::winpeapp::
          DirectImageRestoreResumeOutcomeKind::
              completed_checkpoint_retained;
  if (!completed || !outcome.transfer.has_value() || outcome.rescue_mode ||
      outcome.partial_loss) {
    return false;
  }
  const auto& transfer = *outcome.transfer;
  return !transfer.rescue_mode && !transfer.partial_loss &&
      transfer.final_verified_logical_bytes != 0U &&
      transfer.final_verified_chunk_count != 0U &&
      (outcome.expected_logical_bytes == 0U ||
       transfer.final_verified_logical_bytes ==
           outcome.expected_logical_bytes) &&
      transfer.full_image_reverified_on_same_handle_before_first_write &&
      transfer.target_and_incomplete_layout_reidentified_before_first_write &&
      transfer.verified_prefix_was_not_rewritten &&
      transfer.every_new_chunk_flushed_and_read_back &&
      transfer.final_layout_committed && transfer.target_left_offline;
}

std::wstring format_direct_clone_plan(
    const ytec::winpeapp::DirectCloneOperationPlan& plan) {
  std::wostringstream stream;
  stream
      << L"Y-TEC WinPE 直接クローン（読取り専用の確認結果）\r\n"
         L"判定: ディスク選択条件を通過\r\n\r\n"
         L"コピー元\r\n  ディスク "
      << plan.expected_source.disk_number << L" / "
      << plan.expected_source.model << L"\r\n  容量: "
      << plan.expected_source.size_bytes << L" bytes\r\n  接続: "
      << plan.source_bus_type << L" / 形式: "
      << ytec::diskmodel::partition_style_name(plan.source_partition_style)
      << L"\r\n  シリアル末尾: "
      << utf8_to_wide(plan.expected_source.serial_suffix)
      << L"\r\n  パーティション数: "
      << plan.source_partition_count << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(plan.source_health)
      << L"\r\n\r\nコピー先（確認後に全内容を消去）\r\n  ディスク "
      << plan.expected_target.disk_number << L" / "
      << plan.expected_target.model << L"\r\n  容量: "
      << plan.expected_target.size_bytes << L" bytes\r\n  接続: "
      << plan.target_bus_type << L"\r\n  シリアル末尾: "
      << utf8_to_wide(plan.expected_target.serial_suffix)
      << L"\r\n  削除対象パーティション数: "
      << plan.target_partition_count << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(plan.target_health);
  if (ytec::diskmodel::disk_health_operation_advice(
          plan.source_health, true) ==
      ytec::diskmodel::DiskHealthOperationAdvice::recommend_rescue) {
    stream
        << L"\r\n\r\n【コピー元の健康警告】SMART/NVMeが注意・異常を報告しています。"
           L"救出モードを推奨します。通常モードを続ける場合は、この警告を含む内容を確認してください。";
  }
  if (plan.source_health.temperature_warning ||
      plan.target_health.temperature_warning) {
    stream
        << L"\r\n\r\n【温度警告】冷却状態を確認してください。"
           L"温度だけでは自動停止しません。";
  }
  stream
      << L"\r\n\r\nこの確認ではディスクを変更していません。"
         L"実行時は、この確認結果の安定識別情報を再照合します。";
  return stream.str();
}

std::wstring format_direct_clone_result(
    const ytec::winpeapp::CloneExecutionReport& report) {
  std::wostringstream stream;
  stream
      << L"直接クローンと読戻し検証が完了しました。\r\n"
         L"形式: "
      << (report.partition_style == ytec::winpeapp::ClonePartitionStyle::gpt
              ? L"GPT"
              : L"MBR")
      << L"\r\nコピー済み: " << report.copied_data_bytes
      << L" bytes\r\nコピー済みパーティション: "
      << report.copied_partition_count
      << L"\r\n読戻し検証: "
      << (report.read_back_verified ? L"合格" : L"未完了");
  return stream.str();
}

std::wstring_view direct_shrink_surplus_text(
    const ytec::migrationcore::ShrinkSurplusAllocation allocation) noexcept {
  switch (allocation) {
    case ytec::migrationcore::ShrinkSurplusAllocation::automatic_proportional:
      return L"自動配分（最小必要量確保後、元サイズ比）";
    case ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated:
      return L"未割当のまま";
    case ytec::migrationcore::ShrinkSurplusAllocation::selected_data_partition:
      return L"指定したNTFSデータ領域へ配分";
  }
  return L"不明（実行不可）";
}

std::uint64_t direct_shrink_exact_raw_task_count(
    const ytec::directshrink::TargetPlan& plan) noexcept {
  return static_cast<std::uint64_t>(std::count_if(
      plan.tasks().begin(),
      plan.tasks().end(),
      [](const ytec::directshrink::PartitionTask& task) {
        return task.kind ==
            ytec::directshrink::PartitionTaskKind::copy_exact_raw;
      }));
}

std::wstring format_winpe_offline_direct_shrink_plan(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan& plan) {
  const auto& target = plan.target_plan();
  std::wostringstream stream;
  stream
      << L"Y-TEC WinPE 縮小移行クローン（再解析・計画確定済み）\r\n"
         L"コピー元はOSレベルread-onlyに固定済みで、完了・失敗後も自動解除しません。\r\n\r\n"
         L"コピー元（変更しません）\r\n  ディスク "
      << target.expected_source().disk_number << L" / "
      << target.expected_source().model << L"\r\n  容量: "
      << ytec::winpeapp::format_dashboard_capacity(
             target.expected_source().size_bytes)
      << L" / GPT / 512-byte logical sector"
         L"\r\n  シリアル末尾: "
      << utf8_to_wide(target.expected_source().serial_suffix)
      << L"\r\n\r\nコピー先（確認後に全内容を消去）\r\n  ディスク "
      << target.expected_target().disk_number << L" / "
      << target.expected_target().model << L"\r\n  容量: "
      << ytec::winpeapp::format_dashboard_capacity(
             target.expected_target().size_bytes)
      << L" / 最終形式: GPT"
         L"\r\n  シリアル末尾: "
      << utf8_to_wide(target.expected_target().serial_suffix)
      << L"\r\n\r\nパーティション選択（source table index）";
  for (const auto& mapping : target.source_partition_mappings()) {
    stream << L"\r\n  #" << mapping.source_table_index << L"  "
           << direct_shrink_partition_role_text(mapping.role) << L"  ";
    if (mapping.selected) {
      stream << (mapping.required ? L"選択・必須" : L"選択");
      if (mapping.target_number.has_value()) {
        stream << L" → target #" << *mapping.target_number;
      }
    } else {
      stream << L"除外（コピーしない）";
    }
  }
  stream << L"\r\n\r\n余剰容量: "
         << direct_shrink_surplus_text(target.surplus_allocation());
  if (target.surplus_target_source_table_index().has_value()) {
    stream << L" / source #"
           << *target.surplus_target_source_table_index();
  }
  stream
      << L"\r\n一時WIM: コピー先の非起動・専用staging領域（最大 "
      << ytec::winpeapp::format_dashboard_capacity(
             target.maximum_archive_upper_bound_bytes())
      << L"）"
         L"\r\n一時WIMがコピー先の専用領域に収まらない場合は安全に中止します。"
         L"\r\n未対応FSの元サイズexact RAW: "
      << direct_shrink_exact_raw_task_count(target)
      << L"領域（source/target全chunkのSHA-256・flush・読戻しを検証）"
         L"\r\n起動再構築: "
      << (target.boot_finalization_required()
              ? L"対象側だけで実行し、Boot/WinRE evidenceを読戻し確認"
              : L"対象外（データ専用選択）")
      << L"\r\n最終確定: 全payload・filesystem・Boot証拠の検証後にGPTをcommit last"
         L"\r\n失敗・取消: 未完成GPTを公開せず、コピー先をoffline保持"
         L"\r\n\r\nこの計画ではコピー先をまだ変更していません。"
         L"対象要約を確認し、次画面で大文字 OK を完全一致で入力してください。";
  return stream.str();
}

bool winpe_offline_direct_shrink_completion_verified(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan& plan,
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkOperationReport& report) {
  if (report.lifecycle.outcome !=
          ytec::operationcore::OperationOutcome::completed ||
      !report.execution.has_value()) {
    return false;
  }
  const auto& execution = *report.execution;
  const auto& boot = execution.boot;
  const auto& commit = execution.final_commit;
  const std::uint64_t expected_exact_raw =
      direct_shrink_exact_raw_task_count(plan.target_plan());
  const bool checkpoint_retirement_exactly_one =
      commit.checkpoint_retired != commit.checkpoint_retirement_pending;
  const bool boot_verified =
      (!boot.required ||
       (boot.completed && boot.boot_files_read_back_verified &&
        boot.recovery_configuration_verified)) &&
      boot.target_offline && boot.target_only_reconstruction &&
      boot.exact_target_volume_extents && boot.real_boot_not_claimed;
  return execution.applied_archive_count ==
          plan.target_plan().archive_task_count() &&
      execution.copied_exact_raw_count == expected_exact_raw &&
      execution.source_epoch_revalidation_count ==
          execution.applied_archive_count + expected_exact_raw + 2U &&
      execution.source_locked_read_only_before_target_io &&
      execution.every_capture_used_exact_read_only_lease &&
      execution.source_epoch_rechecked_after_boot_before_final_commit &&
      execution.construction_layout_non_bootable &&
      execution.durable_checkpoint_preceded_payload &&
      execution.final_gpt_committed_last && execution.source_left_os_read_only &&
      execution.target_left_offline && execution.real_boot_not_proven &&
      boot_verified && commit.source_reidentified &&
      commit.source_layout_unchanged && commit.target_reidentified &&
      commit.staging_identity_reverified && commit.checkpoint_reverified &&
      commit.staging_removed && checkpoint_retirement_exactly_one &&
      commit.construction_layout_non_bootable &&
      commit.checkpoint_retained_through_extensions_and_boot &&
      commit.boot_completed_before_final_layout_publication &&
      commit.final_layout_published_before_checkpoint_retirement &&
      commit.hidden_final_layout_published_and_read_back &&
      commit.every_required_ntfs_extension_verified &&
      commit.every_write_flushed && commit.every_write_read_back &&
      commit.primary_layout_committed_last && commit.target_offline &&
      commit.final_partition_style ==
          ytec::migrationcore::MigrationPartitionStyle::gpt;
}

std::wstring format_winpe_offline_direct_shrink_result(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkOperationReport& report) {
  const auto& execution = *report.execution;
  std::wostringstream stream;
  stream
      << L"WinPE 縮小移行クローンと全必須証拠の検証が完了しました。\r\n"
         L"適用したNTFS archive: "
      << execution.applied_archive_count
      << L"\r\n元サイズexact RAWコピー: "
      << execution.copied_exact_raw_count
      << L"\r\n検証済みtarget bytes: " << execution.verified_target_bytes
      << L"\r\nsource epoch再照合: "
      << execution.source_epoch_revalidation_count
      << L"回（初回・各WIM capture／RAW source直前・最終commit直前）"
         L"\r\n全書込みflush＋読戻し: 合格"
         L"\r\nBoot/WinRE evidence: "
      << (execution.boot.required ? L"合格" : L"対象外")
      << L"\r\n最終GPT commit last: 合格"
         L"\r\nコピー元: OS-level read-onlyのまま"
         L"\r\nコピー先: offline（検証完了・換装待ち）"
         L"\r\n実機での起動成功を確認した表示ではありません。";
  return stream.str();
}

std::wstring format_mbr2gpt_clone_plan(
    const ytec::winpeapp::Mbr2GptDirectOperationPlan& plan) {
  const auto& clone = plan.clone;
  std::wostringstream stream;
  stream
      << L"Y-TEC WinPE MBR→GPT（別ディスク／読取り専用の確認結果）\r\n"
         L"判定: 現在接続中の狭い対応条件を通過\r\n\r\n"
         L"コピー元（変更しません）\r\n  ディスク "
      << clone.expected_source.disk_number << L" / "
      << clone.expected_source.model << L"\r\n  容量: "
      << clone.expected_source.size_bytes << L" bytes\r\n  接続: "
      << clone.source_bus_type << L" / 形式: MBR\r\n  シリアル末尾: "
      << utf8_to_wide(clone.expected_source.serial_suffix)
      << L"\r\n  基本プライマリ領域: "
      << plan.primary_partition_count
      << L" / Activeシステム領域: パーティション "
      << plan.active_system_partition_number
      << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(clone.source_health)
      << L"\r\n\r\nコピー先（全内容を消去しGPTへ変換）\r\n  ディスク "
      << clone.expected_target.disk_number << L" / "
      << clone.expected_target.model << L"\r\n  容量: "
      << clone.expected_target.size_bytes << L" bytes\r\n  接続: "
      << clone.target_bus_type << L"\r\n  シリアル末尾: "
      << utf8_to_wide(clone.expected_target.serial_suffix)
      << L"\r\n  削除対象パーティション数: "
      << clone.target_partition_count << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(clone.target_health)
      << L"\r\n\r\nこの確認ではディスクを変更していません。"
         L"実行時は安定識別を再照合し、コピー元をread-onlyに固定してMBR複製を検証後、"
         L"コピー先だけをMicrosoft署名済みMBR2GPTで変換します。"
         L"救出・縮小とは同時に実行しません。";
  return stream.str();
}

std::wstring format_mbr2gpt_clone_result(
    const ytec::winpeapp::Mbr2GptDirectExecutionReport& report) {
  std::wostringstream stream;
  stream
      << L"別ディスクへのMBRクローン、GPT変換、UEFI起動再構築を完了しました。\r\n"
         L"コピー済み: "
      << report.clone.copied_data_bytes
      << L" bytes\r\nコピー済みパーティション: "
      << report.clone.copied_partition_count
      << L"\r\nMBR2GPT /validate: "
      << report.conversion.validation.exit_code
      << L" /convert: " << report.conversion.conversion.exit_code
      << L"\r\nESP/MSR/UEFI BCD: 検証済み"
         L"\r\nコピー元: 安定識別を維持・read-only"
         L"\r\nコピー先: GPT・offline（検証完了・換装待ち）";
  return stream.str();
}

std::wstring format_rescue_clone_plan(
    const ytec::winpeapp::RescueCloneOperationPlan& plan) {
  std::wostringstream stream;
  stream
      << L"Y-TEC WinPE 救出クローン（読取り専用の確認結果）\r\n"
         L"結果分類: "
      << ytec::winpeapp::kRescueCloneResultClassification
      << L"\r\n\r\nコピー元\r\n  ディスク "
      << plan.expected_source.disk_number << L" / "
      << plan.expected_source.model << L"\r\n  容量: "
      << plan.expected_source.size_bytes << L" bytes\r\n  接続: "
      << plan.source_bus_type << L" / 形式: "
      << ytec::diskmodel::partition_style_name(plan.source_partition_style)
      << L"\r\n  シリアル末尾: "
      << utf8_to_wide(plan.expected_source.serial_suffix)
      << L"\r\n  パーティション数: "
      << plan.source_partition_count << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(plan.source_health)
      << L"\r\n\r\nコピー先（確認後に全内容を消去）\r\n  ディスク "
      << plan.expected_target.disk_number << L" / "
      << plan.expected_target.model << L"\r\n  容量: "
      << plan.expected_target.size_bytes << L" bytes\r\n  接続: "
      << plan.target_bus_type << L"\r\n  シリアル末尾: "
      << utf8_to_wide(plan.expected_target.serial_suffix)
      << L"\r\n  削除対象パーティション数: "
      << plan.target_partition_count << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(plan.target_health);
  if (plan.source_partition_style ==
          ytec::diskmodel::PartitionStyle::unknown ||
      plan.source_partition_count == 0U) {
    stream
        << L"\r\n\r\n【認識不能レイアウト】パーティション表を安全に解釈できません。"
           L"救出モードではディスク全体をRAW範囲として扱い、縮小・変換・起動修復を行いません。";
  }
  if (plan.expected_target.size_bytes > plan.expected_source.size_bytes) {
    stream
        << L"\r\n\r\n【コピー先の余剰末尾領域】コピー元容量を超える末尾 "
        << plan.expected_target.size_bytes - plan.expected_source.size_bytes
        << L" bytes は救出RAW範囲外です。既存の生データが残る可能性があるため、"
           L"コピー先はofflineのまま保持し、通常クローン完了として扱いません。";
  }
  stream
      << L"\r\n\r\n救出手順: 前方読取り → 失敗ブロックを逆方向再試行 → "
         L"論理セクター単位で最終再試行"
         L"\r\n未読範囲: ゼロ埋めし、セクター単位の実欠損mapへ記録"
         L"\r\n安全境界: 起動中レスキューUSBを除外 / コピー元をread-onlyのまま保持 / "
         L"コピー先をofflineのまま保持 / 全書込みを直後に読戻し検証"
         L"\r\n禁止事項: 縮小なし / MBR・GPT変換なし / 起動修復・起動完了判定なし"
         L"\r\n今回の製品接続: 512-byte logical sectorのみ（4Knはfail-closed）"
         L"\r\n\r\n救出モードはPE専用です。実行直前にも安定識別情報とレイアウトを再照合します。"
         L"全範囲を回復できても、通常成功ではなく上記の救出結果として表示します。";
  return stream.str();
}

std::wstring format_direct_image_review(
    const ytec::diskmodel::DiskInfo& source,
    const std::wstring_view destination,
    const bool replace_existing,
    const bool encrypted,
    const ytec::imageformat::TsumugiCreateVerificationMode
        verification_mode,
    const bool rescue_mode) {
  std::wostringstream stream;
  stream
      << L"Y-TEC WinPE 直接イメージ作成（読取り専用の確認結果）\r\n"
         L"作成元: ディスク "
      << source.disk_number << L" / " << source.model
      << L"\r\n容量: " << source.size_bytes
      << L" bytes / 形式: "
      << ytec::diskmodel::partition_style_name(source.partition_style)
      << L" / パーティション: " << source.partitions.size()
      << L"\r\nシリアル末尾: " << utf8_to_wide(source.serial_suffix)
      << L"\r\n健康状態: "
      << ytec::winpeapp::format_dashboard_health(source)
      << L"\r\n保存先: " << destination
      << L"\r\n既存完成ファイル: "
      << (replace_existing
              ? L"選択済み検証後に回復可能な手順で置換"
              : L"置換なし")
      << L"\r\n\r\nモード: "
      << (rescue_mode
              ? L"救出（一部欠損の可能性あり）"
              : L"通常（exact）")
      << L"\r\n作成後の検証: "
      << image_create_verification_mode_name(verification_mode)
      << L"\r\n暗号化: "
      << (encrypted
              ? L"あり（Argon2id / AES-256-GCM、回復キーなし）"
              : L"なし")
      << L"\r\nこの画面はexact／救出イメージ作成専用です。"
         L"縮小移行クローンは「ドライブをクローン」の方式選択から直接実行します。"
         L"\r\n予約ジョブは作成しません。"
         L"\r\n実行時に同じディスクを再識別し、コピー元をread-onlyに固定します。"
         L"\r\n各書込み直後の読戻し、認証・Hash、最終メタデータ検証後だけ"
         L"完成名へ確定します。";
  if (verification_mode ==
      ytec::imageformat::TsumugiCreateVerificationMode::complete) {
    stream << L"\r\n完全検証では、完成前に追加の全走査も実行します。";
  } else {
    stream << L"\r\n高速検証では、上記の安全確認を維持し、"
              L"完成前の追加全走査だけを省略します。";
  }
  if (rescue_mode) {
    stream
        << L"\r\n救出時は別ディスク上にRAW一時領域と最大画像の両方の空きを確保します。"
           L"\r\n前方・逆方向・セクター再試行とゼロ埋め読戻し後、"
           L"故障Sourceを再読込せず封印済み一時領域だけから画像化します。"
           L"\r\n画像partialの選択済み検証、一時領域破棄、保存先再識別後だけ完成名を公開します。"
           L"\r\n全範囲を読めても結果分類は救出のままで、通常成功とは表示しません。";
  }
  if (ytec::diskmodel::disk_health_operation_advice(source.health, true) ==
          ytec::diskmodel::DiskHealthOperationAdvice::recommend_rescue &&
      !rescue_mode) {
    stream
        << L"\r\n\r\n【コピー元の健康警告】救出モードを推奨します。"
           L"この画面の通常（exact）作成を続ける場合は、最終確認前に状態を再確認してください。";
  }
  if (source.health.temperature_warning) {
    stream
        << L"\r\n【温度警告】冷却状態を確認してください。温度だけでは自動停止しません。";
  }
  return stream.str();
}

std::wstring format_image_partition_selection_review(
    const ytec::diskmodel::ImagePartitionSelection& selection) {
  if (selection.whole_disk) {
    return L"ディスク全体（全パーティション）";
  }
  std::wstring result = L"Partition ";
  for (std::size_t index = 0U;
       index < selection.selected_partition_numbers.size(); ++index) {
    if (index != 0U) {
      result += L", ";
    }
    const auto number = selection.selected_partition_numbers[index];
    result += L"#" + std::to_wstring(number);
    if (std::find(
            selection.required_partition_numbers.begin(),
            selection.required_partition_numbers.end(),
            number) != selection.required_partition_numbers.end()) {
      result += L"（Windows必須）";
    }
  }
  result += L" / 合計 " + std::to_wstring(selection.selected_bytes) +
      L" bytes";
  return result;
}

std::wstring canonical_image_partition_selection_binding(
    const ytec::diskmodel::ImagePartitionSelection& selection) {
  if (selection.whole_disk) {
    return L"partitions=whole";
  }
  std::wstring result = L"partitions=";
  for (std::size_t index = 0U;
       index < selection.selected_partition_numbers.size(); ++index) {
    if (index != 0U) {
      result.push_back(L',');
    }
    result += std::to_wstring(selection.selected_partition_numbers[index]);
  }
  return result;
}

std::optional<ytec::diskmodel::ImagePartitionSelection>
prompt_image_partition_selection(
    const HWND owner,
    const ytec::diskmodel::DiskInfo& source) {
  const int scope = MessageBoxW(
      owner,
      L"イメージへ保存する範囲を選択します。\r\n\r\n"
      L"[はい] ディスク全体（既定）\r\n"
      L"[いいえ] パーティションを個別選択\r\n"
      L"[キャンセル] 戻る\r\n\r\n"
      L"Windows領域を選んだ場合、ESP／システム／MSR／必要な回復領域は自動追加され、解除できません。",
      L"イメージ対象の選択",
      MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
  if (scope == IDCANCEL) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> requested;
  if (scope == IDNO) {
    auto partitions = source.partitions;
    std::sort(
        partitions.begin(),
        partitions.end(),
        [](const auto& left, const auto& right) {
          return left.offset_bytes < right.offset_bytes;
        });
    for (const auto& partition : partitions) {
      std::wstring prompt =
          L"Partition #" + std::to_wstring(partition.number) +
          L" をイメージへ含めますか？\r\n\r\n容量: " +
          std::to_wstring(partition.size_bytes) + L" bytes";
      if (!partition.name.empty()) {
        prompt += L"\r\n名前: " + partition.name;
      }
      if (!partition.type.empty()) {
        prompt += L"\r\n種類: " + partition.type;
      }
      const int chosen = MessageBoxW(
          owner,
          prompt.c_str(),
          L"パーティション選択",
          MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
      if (chosen == IDCANCEL) {
        return std::nullopt;
      }
      if (chosen == IDYES) {
        requested.push_back(partition.number);
      }
    }
    if (requested.empty()) {
      MessageBoxW(
          owner,
          L"個別選択では少なくとも1つのパーティションを選んでください。\r\n"
          L"何も選ばない操作をディスク全体として解釈しません。",
          kWindowTitle,
          MB_OK | MB_ICONWARNING);
      return std::nullopt;
    }
  }
  auto normalized = ytec::diskmodel::normalize_image_partition_selection(
      source,
      requested);
  if (!normalized) {
    const std::wstring message =
        L"パーティション選択を確定できません。\r\n\r\n" +
        normalized.error().message;
    MessageBoxW(
        owner,
        message.c_str(),
        kWindowTitle,
        MB_OK | MB_ICONWARNING);
    return std::nullopt;
  }
  return normalized.take_value();
}

std::wstring format_direct_image_result(
    const ytec::winpeapp::DirectImageCreateReport& report) {
  const auto verification_mode = report.image.stream.verification_mode;
  const bool final_complete_scan =
      report.image.stream.final_complete_scan_performed &&
      report.image.complete_verification_passed;
  std::wostringstream stream;
  stream
      << (report.rescue_mode
              ? L"救出.tsumugiイメージの作成と選択済み検証が完了しました。\r\n"
              : L"直接.tsumugiイメージの作成と選択済み検証が完了しました。\r\n")
      << L"保存先: "
      << report.image.stream.final_path
      << L"\r\n元データ範囲: " << report.logical_payload_bytes
      << L" bytes\r\n完成ファイル: " << report.image.stream.image_length
      << L" bytes\r\nチャンク: " << report.image.stream.chunk_count
      << L" / パーティション: " << report.imaged_partition_count
      << L"\r\n暗号化: "
      << (report.image.encrypted
              ? L"あり（Argon2id / AES-256-GCM）"
              : L"なし")
      << L"\r\n検証方式: "
      << image_create_verification_mode_name(verification_mode)
      << L"\r\n各書込み読戻し: 合格\r\n認証・Hash: 合格"
         L"\r\n最終メタデータ: 合格\r\n完成前の追加全走査: "
      << (final_complete_scan
              ? verification_mode ==
                        ytec::imageformat::TsumugiCreateVerificationMode::fast
                    ? L"合格（永続再開経路で完全検証へ強化）"
                    : L"合格"
              : L"高速検証のため省略")
      << L"\r\nコピー元: read-onlyのまま保護";
  if (report.rescue_mode && report.rescue.has_value()) {
    stream
        << L"\r\n結果分類: 救出（一部欠損の可能性あり）"
        << L"\r\n実欠損範囲: " << report.rescue->missing_ranges.size()
        << L" / ゼロ埋め: " << report.rescue->zero_filled_bytes
        << L" bytes"
        << L"\r\n一時領域: 封印・選択済み画像検証後の破棄・完成前保存先再識別に合格";
  }
  return stream.str();
}

std::wstring_view restore_mode_name(
    const ytec::imageformat::TsumugiManifestMode mode) noexcept {
  switch (mode) {
    case ytec::imageformat::TsumugiManifestMode::exact:
      return L"通常（exact）";
    case ytec::imageformat::TsumugiManifestMode::rescue:
      return L"救出（一部欠損の可能性あり）";
    case ytec::imageformat::TsumugiManifestMode::shrink:
      return L"縮小移行";
  }
  return L"不明";
}

std::wstring format_verified_restore_image(
    const VerifiedRestoreImageSelection& image) {
  std::wostringstream stream;
  stream
      << L".tsumugi v1 の完全検証に合格しました。\r\n"
         L"ファイル: "
      << image.path << L"\r\nモード: " << restore_mode_name(image.mode)
      << L" / 元ディスク形式: "
      << (image.partition_style ==
                  ytec::imageformat::TsumugiManifestPartitionStyle::gpt
              ? L"GPT"
              : L"MBR")
      << L"\r\n元ディスク容量: "
      << ytec::winpeapp::format_dashboard_capacity(image.source_disk_size)
      << L" / 論理セクター: " << image.logical_sector_size
      << L" bytes / パーティション: " << image.partition_count
      << L"\r\n全チャンク・全体SHA-256: 合格"
      << L"\r\n暗号化: "
      << (image.encrypted
              ? L"あり（入力パスワードで認証済み）"
              : L"なし（パスワード入力なし）");
  if (image.mode == ytec::imageformat::TsumugiManifestMode::shrink) {
    stream
        << L"\r\n\r\n縮小移行はディスク全体だけを復元します。"
           L"個別パーティション復元には切り替えません。"
           L"\r\n次に復元先と、互いに異なるイメージ媒体・起動媒体data作業領域を"
           L"読取り専用で確認します。";
  } else {
    stream
        << L"\r\n\r\n次に復元範囲と復元先を選び、上書き内容を確認してください。"
           L"\r\n既存パーティションへの上書き、または未割当領域への新規1区画作成に対応します。";
  }
  return stream.str();
}

std::wstring format_restore_target_review(
    const VerifiedRestoreImageSelection& image,
    const ytec::diskmodel::DiskInfo& target,
    const std::optional<ytec::imageformat::
        TsumugiPhysicalIndividualPartitionRestoreSelection>& individual) {
  std::wostringstream stream;
  stream
      << format_verified_restore_image(image)
      << L"\r\n\r\n復元先（実行後もオフライン）\r\n  ディスク "
      << target.disk_number << L" / "
      << (target.model.empty() ? L"モデル不明" : target.model)
      << L"\r\n  容量: "
      << ytec::winpeapp::format_dashboard_capacity(target.size_bytes)
      << L" / 接続: "
      << (target.bus_type.empty() ? L"不明" : target.bus_type)
      << L" / 形式: "
      << ytec::diskmodel::partition_style_name(target.partition_style)
      << L"\r\n  シリアル末尾: "
      << (target.serial_suffix.empty()
              ? L"未取得"
              : L"…" + utf8_to_wide(target.serial_suffix))
      << L"\r\n  健康状態: "
      << ytec::winpeapp::format_dashboard_health(target);
  if (individual.has_value()) {
    const auto* existing = std::get_if<ytec::imageformat::
        TsumugiPhysicalExistingPartitionRestoreSelection>(
        &individual->target);
    const auto* unallocated = std::get_if<ytec::imageformat::
        TsumugiPhysicalUnallocatedRestoreSelection>(
        &individual->target);
    const auto source = std::find_if(
        image.manifest.partitions.begin(),
        image.manifest.partitions.end(),
        [&](const ytec::imageformat::TsumugiManifestPartition& partition) {
          return partition.source_table_index ==
              individual->source_table_index;
        });
    if (existing != nullptr && source != image.manifest.partitions.end()) {
      stream
          << L"\r\n\r\n上書き対象: 既存パーティション #"
          << existing->target_partition_number << L" / "
          << ytec::winpeapp::format_dashboard_capacity(
                 existing->target_size)
          << L"\r\n復元元: 画像内パーティション #"
          << source->source_partition_number << L" / "
          << ytec::winpeapp::format_dashboard_capacity(source->source_size)
          << L"\r\n自動バックアップ: なし"
             L"\r\nパーティション表と他の区画: 変更しない";
    } else if (
        unallocated != nullptr && source != image.manifest.partitions.end()) {
      stream
          << L"\r\n\r\n書込み対象: 未割当 offset "
          << ytec::winpeapp::format_dashboard_capacity(
                 unallocated->target_offset)
          << L" / 新規区画 "
          << ytec::winpeapp::format_dashboard_capacity(
                 unallocated->target_size)
          << L"\r\n復元元: 画像内パーティション #"
          << source->source_partition_number << L" / "
          << ytec::winpeapp::format_dashboard_capacity(source->source_size)
          << L"\r\n自動バックアップ: なし"
             L"\r\n既存区画と既存entry: 変更せず、新規1entryだけを最後に確定";
    }
  } else {
    stream
        << L"\r\n\r\n削除対象: ディスク全体（既存パーティションを含む全内容）";
  }
  if (target.partitions.empty()) {
    stream << L"\r\n  ・既存パーティションなし";
  }
  for (const auto& partition : target.partitions) {
    stream << L"\r\n  ・#" << partition.number << L"  offset "
           << ytec::winpeapp::format_dashboard_capacity(
                  partition.offset_bytes)
           << L" / size "
           << ytec::winpeapp::format_dashboard_capacity(
                  partition.size_bytes)
           << L" / "
           << (partition.name.empty() ? partition.type : partition.name);
    if (partition.bootable) {
      stream << L" / Active";
    }
  }
  stream
      << L"\r\n\r\n実行内容: " << restore_mode_name(image.mode)
      << (individual.has_value()
              ? L"の個別パーティション復元、全書込み読戻し検証"
              : L"のディスク全体復元、全書込み読戻し検証")
      << L"\r\n完了状態: 対象はオフラインのまま。起動成功とは表示しません。"
         L"\r\n予約ジョブ・旧 .dcimg/.dcmig は使用しません。"
         L"\r\n\r\n最終確認語: OK";
  return stream.str();
}

std::wstring format_shrink_restore_target_review(
    const VerifiedRestoreImageSelection& image,
    const ytec::diskmodel::DiskInfo& target,
    const ytec::winpeapp::DirectShrinkImageRestoreWorkReview& work,
    const ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout,
    const bool original_source_target) {
  std::wostringstream stream;
  stream
      << format_verified_restore_image(image)
      << L"\r\n\r\n【縮小移行・ディスク全体の上書き確認】"
         L"\r\n復元先: ディスク "
      << target.disk_number << L" / "
      << (target.model.empty() ? L"モデル不明" : target.model)
      << L"\r\n  容量: "
      << ytec::winpeapp::format_dashboard_capacity(target.size_bytes)
      << L" / 接続: "
      << (target.bus_type.empty() ? L"不明" : target.bus_type)
      << L" / 論理セクター: " << target.logical_sector_size
      << L" bytes\r\n  シリアル末尾: "
      << (target.serial_suffix.empty()
              ? L"未取得"
              : L"…" + utf8_to_wide(target.serial_suffix))
      << L"\r\n削除対象: 既存パーティションを含むディスク全体（"
      << target.partitions.size() << L"区画）"
      << L"\r\n最終配置: "
      << (layout.metadata.style ==
                  ytec::imageformat::PartitionTableStyle::gpt
              ? L"GPTを保持"
              : L"MBRを保持")
      << L" / " << layout.migration.target_partitions.size()
      << L"区画 / 512-byte logical sector"
      << L"\r\nイメージ保存元: ディスク "
      << (image.stable_storage_identity.has_value()
              ? std::to_wstring(
                    image.stable_storage_identity->disk_number)
              : L"未確認")
      << L"（復元先と分離済み）"
      << L"\r\n作業媒体: ディスク "
      << work.active_rescue_disk.disk_number
      << L"（イメージ・復元先と分離済み）"
      << L"\r\n  scratch: " << work.paths.scratch_directory
      << L"\r\n  checkpoint: " << work.paths.checkpoint_path
      << L"\r\n  log: " << work.paths.log_path
      << L"\r\n\r\n完全再検証と三つの安定識別を実行直前にもやり直し、"
         L"全書込みをflush・読戻し後、最終区画情報を最後に確定します。"
         L"\r\n予約job・永続resume・個別復元は使用しません。"
         L"対象は完了後もオフラインです。";
  if (original_source_target) {
    stream
        << L"\r\n\r\n【元ディスクへ戻す確認】認証済みsource hashと一致する"
           L"元物理ディスクです。この同じ安定識別を上書き対象として確認しました。";
  }
  stream << L"\r\n\r\n上記の全ディスク消去内容を確認後、最終確認語として大文字の OK を入力してください。";
  return stream.str();
}

std::wstring format_direct_restore_result(
    const ytec::winpeapp::DirectImageRestoreReport& report,
    const bool individual_partition) {
  const auto& physical = report.physical;
  std::wostringstream stream;
  stream
      << (physical.partial_loss
              ? L"救出イメージの復元が完了しました（一部欠損）。"
              : L".tsumugiイメージの復元が完了しました。")
      << L"\r\n書込み済み: "
      << physical.restore.written_logical_bytes
      << L" bytes / チャンク: "
      << physical.restore.written_chunk_count
      << L"\r\n全書込み読戻し: "
      << (physical.restore.all_writes_read_back_verified ? L"合格" : L"未完了")
      << (individual_partition
              ? L"\r\n既存区画の保持と個別配置の最終確定: "
              : L"\r\nパーティション情報の最終確定: ")
      << (physical.restore.final_layout_committed ? L"合格" : L"未完了")
      << L"\r\n復元先: "
      << (physical.target_left_offline ? L"オフラインのまま" : L"要確認")
      << L"\r\n予約ジョブ: 作成していません";
  if (physical.partial_loss) {
    stream
        << L"\r\n\r\nこの結果には読取り不能範囲のゼロ埋めが含まれます。"
           L"完全な復元とは扱わず、欠損マップを確認してください。";
  }
  return stream.str();
}

std::wstring format_direct_shrink_restore_result(
    const ytec::winpeapp::DirectShrinkImageRestoreReport& report) {
  std::wostringstream stream;
  stream
      << L"縮小 .tsumugi のディスク全体復元が完了しました。"
      << L"\r\nNTFS archive: " << report.restore.archive_logical_bytes
      << L" bytes / チャンク: " << report.restore.archive_chunk_count
      << L" / 完了区画: " << report.restore.completed_archive_partitions
      << L"\r\n静的exact RAW: " << report.restore.exact_raw_logical_bytes
      << L" bytes / チャンク: " << report.restore.exact_raw_chunk_count
      << L"\r\n生成区画: "
      << report.restore.intentionally_regenerated_partitions
      << L"\r\n完全再検証・三媒体再識別: 合格"
         L"\r\n全payload書込みflush・読戻し: 合格"
         L"\r\nパーティション情報の最終確定: 合格"
         L"\r\n復元先: オフラインのまま"
         L"\r\n予約job・永続resume: 作成していません";
  if (report.boot_repair_offer_required) {
    stream
        << L"\r\n\r\n最終配置には起動仕上げが必要です。"
           L"続けて起動修復画面で読取り確認してから実行してください。";
  }
  return stream.str();
}

std::wstring format_persistent_restore_result(
    const ytec::winpeapp::DirectImageRestoreResumeOutcome& outcome) {
  const auto& transfer = *outcome.transfer;
  std::wostringstream stream;
  stream
      << (outcome.partial_loss
              ? L"救出イメージの永続再開対応復元が完了しました（一部欠損）。"
              : L".tsumugiイメージの永続再開対応復元が完了しました。")
      << L"\r\n再開時の確認済み位置: "
      << transfer.resumed_verified_logical_bytes << L" bytes / チャンク: "
      << transfer.resumed_verified_chunk_count
      << L"\r\n最終読戻し確認: "
      << transfer.final_verified_logical_bytes << L" bytes / チャンク: "
      << transfer.final_verified_chunk_count
      << L"\r\n全書込み読戻し: "
      << (transfer.every_new_chunk_flushed_and_read_back ? L"合格" : L"未完了")
      << L"\r\nパーティション情報の最終確定: "
      << (transfer.final_layout_committed ? L"合格" : L"未完了")
      << L"\r\n復元先: "
      << (transfer.target_left_offline ? L"オフラインのまま" : L"要確認")
      << L"\r\n予約ジョブ: 作成していません";
  if (outcome.checkpoint_cleanup_error) {
    stream
        << L"\r\n\r\n復元と最終検証は完了しましたが、再開情報の後片付けだけが完了していません。"
           L"次回起動時の破棄確認で安全に削除できます。";
  }
  return stream.str();
}

ytec::clonecore::Result<ytec::operationcore::OperationId>
make_operation_id(const std::wstring_view operation_name) {
  GUID guid{};
  const HRESULT created = CoCreateGuid(&guid);
  if (FAILED(created)) {
    return ytec::clonecore::Result<
        ytec::operationcore::OperationId>::failure({
        .code = ytec::clonecore::ErrorCode::io_failed,
        .native_code = static_cast<DWORD>(created),
        .operation = std::wstring(operation_name),
        .message = L"単回操作IDを生成できません",
    });
  }
  ytec::operationcore::OperationId id{};
  static_assert(sizeof(guid) == 16U);
  static_assert(sizeof(guid) == id.size());
  std::memcpy(id.data(), &guid, id.size());
  return ytec::clonecore::Result<
      ytec::operationcore::OperationId>::success(id);
}

[[nodiscard]] ytec::clonecore::Result<std::unique_ptr<
    ytec::operationcore::IResumeSlotPlatform>>
make_product_resume_slot_platform() {
#ifdef YTEC_UI_ACCEPTANCE_BUILD
  return ytec::clonecore::Result<std::unique_ptr<
      ytec::operationcore::IResumeSlotPlatform>>::failure(
      restore_ui_error(
          ytec::clonecore::ErrorCode::unsupported_platform,
          ERROR_NOT_SUPPORTED,
          L"WinPE SingleResumeSlot product gate",
          L"非破壊UI受入ビルドではhostのportable dataを観測せず、"
          L"新規書込み操作を開始しません"));
#else
  auto storage = ytec::winpeapp::
      make_direct_image_restore_windows_storage_platform_v1();
  if (!storage) {
    return ytec::clonecore::Result<std::unique_ptr<
        ytec::operationcore::IResumeSlotPlatform>>::failure(
        storage.error());
  }
  return ytec::operationcore::
      make_current_executable_windows_resume_slot_platform(
          storage.value().prove_data_backing);
#endif
}

[[nodiscard]] ytec::clonecore::Result<
    ytec::winpeapp::DirectImageCreateResumeStartupObservation>
inspect_product_image_create_resume() {
#ifdef YTEC_UI_ACCEPTANCE_BUILD
  return ytec::clonecore::Result<
      ytec::winpeapp::DirectImageCreateResumeStartupObservation>::failure(
      restore_ui_error(
          ytec::clonecore::ErrorCode::unsupported_platform,
          ERROR_NOT_SUPPORTED,
          L"WinPE image-create persistent startup",
          L"非破壊UI受入ビルドではhostのportable dataを観測しません"));
#else
  auto storage = ytec::winpeapp::
      make_direct_image_create_windows_resume_storage_platform_v1();
  if (!storage) {
    return ytec::clonecore::Result<
        ytec::winpeapp::DirectImageCreateResumeStartupObservation>::failure(
        storage.error());
  }
  auto platform = ytec::operationcore::
      make_current_executable_windows_resume_slot_platform(
          storage.value().prove_data_backing);
  if (!platform) {
    return ytec::clonecore::Result<
        ytec::winpeapp::DirectImageCreateResumeStartupObservation>::failure(
        platform.error());
  }
  return ytec::winpeapp::inspect_direct_image_create_resume_v1(
      *platform.value());
#endif
}

[[nodiscard]] bool guard_new_product_operation_start(
    const HWND owner,
    const HWND output,
    const ytec::clonecore::Result<ytec::operationcore::OperationPlan>&
        admission_plan) {
  if (!admission_plan) {
    set_error_output_and_show(owner, output, admission_plan.error());
    return false;
  }
  auto platform = make_product_resume_slot_platform();
  if (!platform) {
    set_error_output_and_show(owner, output, platform.error());
    return false;
  }
  const auto admitted = ytec::winpeapp::guard_new_winpe_operation_start(
      admission_plan.value(), *platform.value());
  if (!admitted) {
    set_error_output_and_show(
        owner,
        output,
        admitted.error(),
        L"active.checkpointとその所有物は変更していません。"
        L"復元画面で同じbindingの再開または所有物だけの破棄を選んでください。");
    return false;
  }
  return true;
}

[[nodiscard]] bool guard_bound_product_restore_resume(
    const HWND owner,
    const HWND output,
    const ytec::operationcore::ResumeSlotBinding& reviewed_binding) {
  auto platform = make_product_resume_slot_platform();
  if (!platform) {
    set_error_output_and_show(owner, output, platform.error());
    return false;
  }
  const auto admitted = ytec::winpeapp::guard_bound_winpe_restore_resume(
      reviewed_binding, *platform.value());
  if (!admitted) {
    set_error_output_and_show(
        owner,
        output,
        admitted.error(),
        L"表示後にslotが変化したか、完全bindingが一致しません。"
        L"checkpointと所有partialは変更していません。");
    return false;
  }
  return true;
}

std::wstring format_automatic_boot_repair_plan(
    const ytec::bootrepair::AutomaticBootRepairPlan& plan,
    const ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution* const
        review) {
  std::wostringstream stream;
  stream
      << L"自動起動修復（読み取り専用の解析結果）\r\n"
         L"対象: ディスク "
      << plan.selected_disk.disk_number << L" / "
      << plan.selected_disk.model << L"\r\n容量: "
      << ytec::winpeapp::format_dashboard_capacity(
             plan.selected_disk.size_bytes)
      << L" / 形式: "
      << ytec::diskmodel::partition_style_name(plan.partition_style)
      << L" / 起動方式: "
      << (plan.firmware == ytec::bootrepair::BcdBootFirmware::uefi
              ? L"UEFI"
              : L"BIOS")
      << L" / シリアル末尾: "
      << (plan.selected_disk.serial_suffix.empty()
              ? L"未取得"
              : L"…" + utf8_to_wide(plan.selected_disk.serial_suffix))
      << L"\r\n接続: "
      << (plan.selected_disk.bus_type.empty()
              ? L"不明"
              : plan.selected_disk.bus_type)
      << L"\r\nWindows候補: " << plan.windows_installations.size()
      << L" / システム領域候補: "
      << plan.system_partition_candidates.size();
  stream << L"\r\n\r\nパーティション図（変更前）";
  if (plan.selected_disk.partitions.empty()) {
    stream << L"\r\n・パーティションなし";
  }
  for (const auto& partition : plan.selected_disk.partitions) {
    stream
        << L"\r\n・#" << partition.number << L"  offset "
        << ytec::winpeapp::format_dashboard_capacity(
               partition.offset_bytes)
        << L" / size "
        << ytec::winpeapp::format_dashboard_capacity(
               partition.size_bytes)
        << L" / type "
        << (partition.type.empty() ? L"不明" : partition.type);
    if (!partition.name.empty()) {
      stream << L" / " << partition.name;
    }
    if (partition.bootable) {
      stream << L" / Active";
    }
  }
  for (const auto& windows : plan.windows_installations) {
    stream << L"\r\n\r\n・Windows区画 #" << windows.partition.number
           << L" / " << windows.version.major << L"."
           << windows.version.build
           << (windows.officially_supported ? L" / 対応" : L" / 未保証")
           << L" / WinRE登録先: "
           << (windows.winre.registered_location_reported
                   ? windows.winre.registered_location_matches_selected_disk
                         ? L"対象ディスク内"
                         : L"別ディスクまたは不一致"
                   : L"未取得");
  }
  for (const auto& system : plan.system_partition_candidates) {
    stream << L"\r\n・システム区画 #" << system.partition.number
           << L" / " << system.volume.location.file_system << L" / "
           << (system.role ==
                       ytec::bootrepair::BootSystemPartitionRole::efi_system
                    ? L"ESP"
                    : L"Active");
    if (system.role ==
        ytec::bootrepair::BootSystemPartitionRole::efi_system) {
      stream << L" / EFI所有権: ";
      switch (system.efi_ownership.state) {
        case ytec::bootrepair::EfiBootOwnershipState::
            microsoft_only_or_empty:
          stream << L"Microsoftのみ、または空";
          break;
        case ytec::bootrepair::EfiBootOwnershipState::
            non_microsoft_or_untrusted_present:
          stream
              << (ytec::bootrepair::
                          efi_boot_ownership_allows_third_party_preserve(
                              system.efi_ownership)
                      ? L"独立した第三者EFI namespaceを検出（保持修復可）"
                      : L"第三者または未検証を検出（開始禁止）");
          break;
        case ytec::bootrepair::EfiBootOwnershipState::ambiguous:
          stream << L"曖昧（開始禁止）";
          break;
        case ytec::bootrepair::EfiBootOwnershipState::not_applicable:
        default:
          stream << L"未確認（開始禁止）";
          break;
      }
      stream << L" / Microsoft署名EFI: "
             << system.efi_ownership.microsoft_signed_efi_loader_count
             << L" / 非Microsoft・未検証項目: "
             << system.efi_ownership
                    .non_microsoft_or_untrusted_entry_count;
    }
  }
  if (review != nullptr) {
    const auto& first_request =
        review->requests_in_boot_priority.front();
    const bool uefi =
        first_request.firmware ==
        ytec::bootrepair::BcdBootFirmware::uefi;
    const std::wstring bcd_relative = uefi
        ? L"\\EFI\\Microsoft\\Boot\\BCD"
        : L"\\Boot\\BCD";
    stream
        << L"\r\n\r\n実行計画: Microsoft署名済みBCDBoot /cによる"
           L"新規BCD再構築と、表示順のWindows登録";
    for (std::size_t index = 0U;
         index < review->requests_in_boot_priority.size(); ++index) {
      stream << L"\r\n読取り元 " << (index + 1U)
             << L"（書込み対象外）: Windows区画 #"
             << review->windows_partition_numbers_in_boot_priority[index]
             << L" / "
             << review->requests_in_boot_priority[index].windows_root;
    }
    stream
        << L"\r\n起動優先順: ";
    for (std::size_t index = 0U;
         index < review->windows_partition_numbers_in_boot_priority.size();
         ++index) {
      if (index != 0U) {
        stream << L" → ";
      }
      stream << L"#"
             << review->windows_partition_numbers_in_boot_priority[index];
    }
    stream
        << L"\r\n書込み対象: システム区画 #"
        << review->system_partition_number << L" / "
        << (review->temporary_system_mount_required
                ? L"実行中だけ一時ドライブ文字を割当"
                : first_request.system_root)
        << L"\r\n変更内容: 起動ファイルと " << bcd_relative
        << L" を新規再構築"
           L"\r\n退避先: 同じシステム区画内の "
        << bcd_relative
        << L".ytec-rebuild-backup（既存BCDがある場合）"
           L"\r\n非変更: パーティション構成、Active属性、"
           L"BIOSブートコード";
    if (uefi) {
      stream << L"\r\nEFI保護: ";
      if (review->third_party_efi_preserved) {
        stream
            << L"第三者EFIを保持します。削除・改名せず、Microsoftの"
               L"起動領域だけをBCDBoot /sで再構築します。";
      } else if (review->third_party_efi_delete_requested) {
        stream
            << L"削除（危険）を明示済みです。専用reviewで固定した"
               L"EFI直下の独立第三者directoryだけを同一ESP quarantineへ"
               L"handle-bound移動し、BCD readback後にexact identityで削除します。"
               L"Microsoft/Boot/fallback/rootは対象外です。";
      } else {
        stream << L"Microsoftのみ／空と確認済みです。";
      }
      stream
          << L" 実行直前にも同じVolume GUIDとEFI診断を再照合します。"
             L"\r\nNVRAM: ";
      if (review->repair_current_pc_nvram) {
        stream
            << L"「このPCで使用する」を明示済み。BCDBoot /s完了後、"
               L"対象ESPのWindows Boot Managerだけを条件付きで有効化／"
               L"BootOrder末尾追加し、完全読戻しします。";
      } else {
        stream << L"BCDBoot /sを使用し、現在PCのNVRAMは変更しません。";
      }
    }
    const auto winre_registration_count = static_cast<std::size_t>(
        std::count_if(
            review->winre_actions_in_boot_priority.begin(),
            review->winre_actions_in_boot_priority.end(),
            [](const auto& action) {
              return action.disposition == ytec::bootrepair::
                  AutomaticWinReRepairDisposition::
                      register_verified_windows_image;
            }));
    stream << L"\r\nWinRE: ";
    if (winre_registration_count != 0U) {
      stream
          << L"Windows\\System32\\Recovery\\Winre.wimをFile ID・時刻・"
             L"SHA-256で確認し、実行直前に同じファイルをlockして "
          << winre_registration_count
          << L"件を再登録・再診断します。";
    }
    if (review->normal_boot_only_partial) {
      if (winre_registration_count != 0U) {
        stream << L" ";
      }
      stream << L"未確認／欠損候補は変更せず、通常起動だけの部分修復です。";
    } else if (winre_registration_count == 0U) {
      stream << L"既存登録を確認済み。登録内容は変更しません。";
    }
    stream
        << L"\r\n新規BCDの通常ファイル存在を確認し、"
           L"失敗時は既存BCDをロールバックします。"
           L"\r\n予約ジョブは作成しません。";
  }
  return stream.str();
}

std::wstring format_system_partition_creation_review(
    const ytec::bootrepair::ReviewedSystemPartitionCreation& reviewed) {
  const auto& plan = reviewed.discovery();
  const bool uefi = reviewed.system_role() ==
      ytec::bootrepair::BootSystemPartitionRole::efi_system;
  std::wostringstream stream;
  stream
      << L"\r\n\r\nシステム領域の新規作成（独立した追加確認）"
         L"\r\n縮小するWindows区画: #"
      << reviewed.windows_partition().number << L" / "
      << reviewed.windows_volume_name()
      << L"\r\nWindows区画: "
      << ytec::winpeapp::format_dashboard_capacity(
             reviewed.windows_partition().size_bytes)
      << L" → "
      << ytec::winpeapp::format_dashboard_capacity(
             reviewed.shrunken_windows_size_bytes())
      << L"\r\n作成する領域: "
      << (uefi ? L"FAT32 ESP" : L"NTFS Active system partition")
      << L" / offset "
      << ytec::winpeapp::format_dashboard_capacity(
             reviewed.system_partition_offset_bytes())
      << L" / size "
      << ytec::winpeapp::format_dashboard_capacity(
             reviewed.system_partition_size_bytes())
      << L"\r\n書込み対象: ディスク "
      << plan.selected_disk.disk_number << L" のWindows区画終端と新規区画"
         L"\r\n危険: NTFS縮小、パーティション表更新、"
      << (uefi ? L"FAT32 format" : L"NTFS formatとActive属性設定")
      << L"を行います。電源断や装置切断を避けてください。"
         L"\r\n安全策: 実行直前の安定再識別、全区画・Volume GUID・"
         L"最大縮小可能量の完全再確認、各段階の読戻し、失敗時のexact rollback"
         L"\r\nこの段階ではBCD、WinRE、第三者EFI、現在PCのNVRAMを変更しません。"
         L"作成完了後に再解析し、別の起動修復レビューと大文字OKを要求します。"
         L"\r\n予約ジョブは作成しません。"
         L"\r\n\r\n上記の追加確認に同意する場合だけ、大文字 OK を入力してください。";
  return stream.str();
}

std::wstring format_system_partition_creation_report(
    const ytec::bootrepair::SystemPartitionCreationReport& report) {
  using ytec::bootrepair::SystemPartitionCreationOutcome;
  std::wostringstream stream;
  switch (report.outcome) {
    case SystemPartitionCreationOutcome::committed:
      stream
          << L"システム領域の準備トランザクションが完了しました。"
             L"\r\n・追加の大文字OK: 確認済み"
             L"\r\n・対象の実行直前再識別: 完全一致"
             L"\r\n・Windows NTFS縮小: exact寸法を読戻し済み"
             L"\r\n・新規ESP／Active領域: 作成、format、役割、Volume GUIDを読戻し済み"
             L"\r\n\r\nこれは起動修復の完了や起動成功を示しません。"
             L"続けて新しいレイアウトを再解析し、BCD／WinRE／NVRAMの"
             L"別レビューを行います。";
      break;
    case SystemPartitionCreationOutcome::rolled_back_exact:
      stream
          << L"システム領域の作成は完了しませんでしたが、"
             L"縮小前のWindows extentと全レイアウトへexact rollback済みです。";
      break;
    case SystemPartitionCreationOutcome::rollback_incomplete:
      stream
          << L"システム領域の作成に失敗し、元のレイアウトへ戻ったことも"
             L"完全確認できませんでした。これ以上の自動書込みを停止しました。"
             L"\r\n対象ディスクを再起動・取外しせず、現在の状態を保って確認してください。";
      break;
    case SystemPartitionCreationOutcome::failed_before_mutation:
    default:
      stream
          << L"実行前の追加確認または完全再解析に合格しなかったため、"
             L"システム領域は作成していません。";
      break;
  }
  return stream.str();
}

std::optional<std::uint32_t>
prompt_system_partition_creation_windows_choice(
    const HWND owner,
    const ytec::bootrepair::AutomaticBootRepairPlan& plan) {
  constexpr int kWindowsButtonBaseId = 43400;
  constexpr std::size_t kMaximumWindowsChoices = 32U;
  if (!plan.system_partition_create_plan_needed ||
      !plan.system_partition_candidates.empty() ||
      plan.windows_installations.empty() ||
      plan.windows_installations.size() > kMaximumWindowsChoices) {
    MessageBoxW(
        owner,
        L"システム領域作成の対象Windowsを安全に選べません。ディスクは変更しません。",
        L"システム領域の作成",
        MB_OK | MB_ICONWARNING);
    return std::nullopt;
  }

  std::vector<std::wstring> labels;
  labels.reserve(plan.windows_installations.size());
  for (const auto& windows : plan.windows_installations) {
    std::wostringstream label;
    label << L"Windows区画 #" << windows.partition.number
          << L" の終端を縮小して作成計画を確認\nWindows "
          << windows.version.major << L"." << windows.version.build
          << L" / "
          << ytec::winpeapp::format_dashboard_capacity(
                 windows.partition.size_bytes);
    if (!windows.volume.mount_points.empty()) {
      label << L" / " << windows.volume.mount_points.front();
    }
    labels.push_back(label.str());
  }
  std::vector<TASKDIALOG_BUTTON> buttons;
  buttons.reserve(labels.size());
  for (std::size_t index = 0U; index < labels.size(); ++index) {
    buttons.push_back(TASKDIALOG_BUTTON{
        kWindowsButtonBaseId + static_cast<int>(index),
        labels[index].c_str()});
  }

  const bool uefi = plan.required_system_partition_role ==
      ytec::bootrepair::BootSystemPartitionRole::efi_system;
  const std::wstring content =
      L"既存のシステム領域がないため、通常のBCD修復はまだ開始できません。\r\n"
      L"選択したsimple NTFS Windows volumeをMicrosoft VDSでexact縮小し、" +
      std::wstring(uefi ? L"260 MiB FAT32 ESP" : L"100 MiB NTFS Active領域") +
      L"を新規作成する計画だけを先にレビューします。\r\n\r\n"
      L"この選択ではまだ書き込みません。安全条件と配置を再確認した後、"
      L"次画面で追加の大文字 OK が必要です。";

  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_USE_COMMAND_LINKS |
                   TDF_NO_DEFAULT_RADIO_BUTTON |
                   TDF_SIZE_TO_CONTENT;
  config.pszWindowTitle = L"システム領域作成の追加確認";
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction =
      L"縮小するWindows区画を明示してください";
  config.pszContent = content.c_str();
  config.cButtons = static_cast<UINT>(buttons.size());
  config.pButtons = buttons.data();
  config.nDefaultButton = IDCANCEL;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;

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
  int pressed = IDCANCEL;
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result)) {
    MessageBoxW(
        owner,
        L"システム領域作成の選択画面を開けないため、ディスクは変更しません。",
        L"システム領域の作成",
        MB_OK | MB_ICONWARNING);
    return std::nullopt;
  }
  const int selected_index = pressed - kWindowsButtonBaseId;
  if (selected_index < 0 ||
      static_cast<std::size_t>(selected_index) >=
          plan.windows_installations.size()) {
    return std::nullopt;
  }

  const auto& selected = plan.windows_installations[
      static_cast<std::size_t>(selected_index)];
  std::wostringstream warning;
  warning
      << L"対象: ディスク " << plan.selected_disk.disk_number << L" / "
      << plan.selected_disk.model << L"\r\nWindows区画: #"
      << selected.partition.number << L" / "
      << ytec::winpeapp::format_dashboard_capacity(
             selected.partition.size_bytes)
      << L"\r\n作成予定: "
      << (uefi ? L"260 MiB FAT32 ESP" : L"100 MiB NTFS Active領域")
      << L"\r\n\r\nNTFS縮小とパーティション表更新を伴います。"
         L"この対象で読み取り専用の詳細レビューへ進みますか？"
         L"\r\n（ここで「はい」を選んでも、まだ書き込みません）";
  if (MessageBoxW(
          owner,
          warning.str().c_str(),
          L"システム領域作成の追加確認",
          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return std::nullopt;
  }
  return selected.partition.number;
}

std::optional<ytec::winpeapp::WinPeAutomaticBootRepairProductChoice>
prompt_automatic_boot_repair_windows_choice(
    const HWND owner,
    const ytec::bootrepair::AutomaticBootRepairPlan& plan) {
  constexpr int kAllWindowsButtonId = 43000;
  constexpr int kSelectedWindowsButtonBaseId = 43100;
  constexpr std::size_t kMaximumWindowsChoices = 32U;
  if (plan.windows_installations.empty() ||
      plan.windows_installations.size() > kMaximumWindowsChoices) {
    MessageBoxW(
        owner,
        L"Windows候補件数が選択画面の安全上限外です。修復は開始しません。",
        L"起動修復のWindows選択",
        MB_OK | MB_ICONWARNING);
    return std::nullopt;
  }

  std::wostringstream priority;
  for (std::size_t index = 0U;
       index < plan.windows_installations.size(); ++index) {
    if (index != 0U) {
      priority << L" → ";
    }
    priority << L"#"
             << plan.windows_installations[index].partition.number;
  }
  std::wstring content =
      L"全て登録する場合、次の表示順が起動優先順になります。\r\n" +
      priority.str() +
      L"\r\n\r\n選択した内容は最終確認まで保持し、実行直前に再解析します。"
      L"UEFIでは次画面で現在PCのNVRAM方針も明示します。安全に特定したWindows内"
      L"Winre.wimだけは別の固定トランザクションで登録します。";
  const bool partial = std::any_of(
      plan.windows_installations.begin(),
      plan.windows_installations.end(),
      [](const auto& windows) {
        return windows.winre.source_state ==
                   ytec::bootrepair::WinReSourceState::missing ||
            windows.winre.source_state ==
                   ytec::bootrepair::WinReSourceState::unknown;
      });
  if (partial) {
    content +=
        L"\r\n\r\nWinREを確認できない候補は、通常起動だけの部分修復として"
        L"明示し、WinREを完了扱いにしません。";
  }
  const bool preserve_third_party = std::any_of(
      plan.system_partition_candidates.begin(),
      plan.system_partition_candidates.end(),
      [](const auto& system) {
        return ytec::bootrepair::
            efi_boot_ownership_allows_third_party_preserve(
                system.efi_ownership);
      });
  if (preserve_third_party) {
    content +=
        L"\r\n\r\n第三者EFIは保持します。削除・改名せず、Microsoftの"
        L"起動領域だけをBCDBoot /sで再構築します。";
  }

  std::vector<std::wstring> labels;
  labels.reserve(plan.windows_installations.size() + 1U);
  if (plan.windows_installations.size() > 1U) {
    labels.push_back(
        L"全て登録（起動優先順: " + priority.str() + L"）");
  }
  for (const auto& windows : plan.windows_installations) {
    std::wostringstream label;
    label << L"Windows区画 #" << windows.partition.number
          << L" だけ登録\nWindows " << windows.version.major
          << L"." << windows.version.build << L" / "
          << ytec::winpeapp::format_dashboard_capacity(
                 windows.partition.size_bytes)
          << (windows.officially_supported ? L" / 対応" : L" / 未保証");
    if (!windows.volume.mount_points.empty()) {
      label << L" / " << windows.volume.mount_points.front();
    }
    labels.push_back(label.str());
  }

  std::vector<TASKDIALOG_BUTTON> buttons;
  buttons.reserve(labels.size());
  std::size_t label_index = 0U;
  if (plan.windows_installations.size() > 1U) {
    buttons.push_back(TASKDIALOG_BUTTON{
        kAllWindowsButtonId, labels[label_index++].c_str()});
  }
  for (std::size_t index = 0U;
       index < plan.windows_installations.size(); ++index) {
    buttons.push_back(TASKDIALOG_BUTTON{
        kSelectedWindowsButtonBaseId + static_cast<int>(index),
        labels[label_index++].c_str()});
  }

  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_USE_COMMAND_LINKS |
                   TDF_NO_DEFAULT_RADIO_BUTTON |
                   TDF_SIZE_TO_CONTENT;
  config.pszWindowTitle = L"起動修復のWindows選択";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction =
      L"登録するWindowsと起動優先順を選択してください";
  config.pszContent = content.c_str();
  config.cButtons = static_cast<UINT>(buttons.size());
  config.pButtons = buttons.data();
  // Escape and the window close button cancel. No write-capable choice is the
  // default, so Enter cannot silently approve a discovered priority.
  config.nDefaultButton = IDCANCEL;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;

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
  int pressed = IDCANCEL;
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result)) {
    MessageBoxW(
        owner,
        L"Windows選択画面を開けないため、起動修復は開始しません。",
        L"起動修復のWindows選択",
        MB_OK | MB_ICONWARNING);
    return std::nullopt;
  }
  ytec::winpeapp::WinPeAutomaticBootRepairProductChoice choice{
      .explicitly_approved = true,
  };
  if (pressed == kAllWindowsButtonId &&
      plan.windows_installations.size() > 1U) {
    choice.windows_policy = ytec::bootrepair::
        AutomaticWindowsRegistrationPolicy::all_with_explicit_priority;
    for (const auto& windows : plan.windows_installations) {
      choice.windows_partition_priority.push_back(
          windows.partition.number);
    }
    return choice;
  }
  const int selected_index = pressed - kSelectedWindowsButtonBaseId;
  if (selected_index < 0 ||
      static_cast<std::size_t>(selected_index) >=
          plan.windows_installations.size()) {
    return std::nullopt;
  }
  choice.windows_policy = ytec::bootrepair::
      AutomaticWindowsRegistrationPolicy::selected_only;
  choice.windows_partition_priority.push_back(
      plan.windows_installations[static_cast<std::size_t>(selected_index)]
          .partition.number);
  return choice;
}

std::optional<ytec::winpeapp::WinPeAutomaticBootRepairProductChoice>
prompt_automatic_boot_repair_efi_choice(
    const HWND owner,
    const ytec::bootrepair::AutomaticBootRepairPlan& plan,
    ytec::winpeapp::WinPeAutomaticBootRepairProductChoice choice) {
  if (!ytec::winpeapp::
          automatic_boot_repair_allows_third_party_efi_delete(plan)) {
    return choice;
  }
  constexpr int kPreserveButtonId = 43300;
  constexpr int kDeleteButtonId = 43301;
  const std::array<TASKDIALOG_BUTTON, 2> buttons{{
      {kPreserveButtonId,
       L"保持（既定）\n第三者EFIは変更せずMicrosoft起動領域だけを修復"},
      {kDeleteButtonId,
       L"削除（危険）\n専用manifest review後に第三者EFI namespaceを削除"},
  }};
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_USE_COMMAND_LINKS |
                   TDF_SIZE_TO_CONTENT;
  config.pszWindowTitle = L"第三者EFIの扱い";
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction =
      L"第三者EFIは保持が既定です。必要な場合だけ削除を選択してください";
  config.pszContent =
      L"削除できるのはEFI直下の独立した非Microsoft通常directoryだけです。"
      L"Microsoft、Boot、fallback、曖昧object、未信頼loaderは削除対象にできません。\r\n\r\n"
      L"削除を選ぶと、全entryのFileId・size・times・SHA-256を専用レビューし、"
      L"大文字 OK と実行直前のexact再照合後だけtransactionを開始します。";
  config.cButtons = static_cast<UINT>(buttons.size());
  config.pButtons = buttons.data();
  config.nDefaultButton = kPreserveButtonId;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;

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
  int pressed = IDCANCEL;
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result) || pressed == IDCANCEL) {
    if (FAILED(result)) {
      MessageBoxW(
          owner,
          L"第三者EFI方針を安全に選べないため、起動修復は開始しません。",
          L"第三者EFIの扱い",
          MB_OK | MB_ICONWARNING);
    }
    return std::nullopt;
  }
  if (pressed == kPreserveButtonId) {
    choice.third_party_efi_policy =
        ytec::bootrepair::AutomaticThirdPartyEfiPolicy::preserve;
    choice.third_party_efi_delete_explicitly_approved = false;
    return choice;
  }
  if (pressed == kDeleteButtonId) {
    choice.third_party_efi_policy = ytec::bootrepair::
        AutomaticThirdPartyEfiPolicy::delete_non_microsoft;
    choice.third_party_efi_delete_explicitly_approved = true;
    return choice;
  }
  return std::nullopt;
}

std::optional<ytec::winpeapp::WinPeAutomaticBootRepairProductChoice>
prompt_automatic_boot_repair_nvram_choice(
    const HWND owner,
    const ytec::bootrepair::AutomaticBootRepairPlan& plan,
    ytec::winpeapp::WinPeAutomaticBootRepairProductChoice choice) {
  if (plan.firmware != ytec::bootrepair::BcdBootFirmware::uefi) {
    return choice;
  }
  constexpr int kLeaveUnchangedButtonId = 43200;
  constexpr int kRepairCurrentPcButtonId = 43201;
  const std::array<TASKDIALOG_BUTTON, 2> buttons{{
      {kLeaveUnchangedButtonId,
       L"NVRAMを変更しない（推奨）\nBCDBoot /sだけで別PC向けの起動ファイルを修復"},
      {kRepairCurrentPcButtonId,
       L"このPCで使用するためNVRAMも修復\n対象ESPのWindows Boot Managerだけを確認・末尾登録"},
  }};
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                   TDF_POSITION_RELATIVE_TO_WINDOW |
                   TDF_USE_COMMAND_LINKS |
                   TDF_SIZE_TO_CONTENT;
  config.pszWindowTitle = L"現在PCのUEFI NVRAM方針";
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszMainInstruction =
      L"修復したディスクをこのPCで使うか選択してください";
  config.pszContent =
      L"NVRAM修復を選ぶと、対象ESPを指すWindows Boot Managerを条件付きで"
      L"有効化または既存BootOrderの末尾へ追加します。既存順序は変更しません。\r\n\r\n"
      L"この選択は最終計画へ固定され、実行前にもう一度対象ディスクとESPを"
      L"完全再確認します。書込み開始には次画面で大文字 OK も必要です。";
  config.cButtons = static_cast<UINT>(buttons.size());
  config.pButtons = buttons.data();
  config.nDefaultButton = kLeaveUnchangedButtonId;
  config.dwCommonButtons = TDCBF_CANCEL_BUTTON;

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
  int pressed = IDCANCEL;
  const HRESULT result = task_dialog == nullptr
      ? HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
      : task_dialog(&config, &pressed, nullptr, nullptr);
  if (common_controls != nullptr) {
    FreeLibrary(common_controls);
  }
  if (FAILED(result) || pressed == IDCANCEL) {
    if (FAILED(result)) {
      MessageBoxW(
          owner,
          L"NVRAM方針を安全に選べないため、起動修復は開始しません。",
          L"現在PCのUEFI NVRAM方針",
          MB_OK | MB_ICONWARNING);
    }
    return std::nullopt;
  }
  if (pressed == kRepairCurrentPcButtonId) {
    choice.nvram_policy = ytec::bootrepair::AutomaticNvramRepairPolicy::
        repair_current_pc_windows_boot_manager;
    choice.current_pc_nvram_explicitly_approved = true;
    return choice;
  }
  if (pressed != kLeaveUnchangedButtonId) {
    return std::nullopt;
  }
  choice.nvram_policy =
      ytec::bootrepair::AutomaticNvramRepairPolicy::leave_unchanged;
  choice.current_pc_nvram_explicitly_approved = false;
  return choice;
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<InventoryPayload> payload) {
  InventoryPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<ImageCreateProgressPayload> payload) {
  ImageCreateProgressPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<ImageCreatePayload> payload) {
  ImageCreatePayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<ImageRestoreVerifyPayload> payload) {
  ImageRestoreVerifyPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<ImageRestoreProgressPayload> payload) {
  ImageRestoreProgressPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<ImageRestorePayload> payload) {
  ImageRestorePayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<CloneCheckPayload> payload) {
  CloneCheckPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<CloneShrinkInspectionPayload> payload) {
  CloneShrinkInspectionPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<CloneProgressPayload> payload) {
  CloneProgressPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<CloneExecutePayload> payload) {
  CloneExecutePayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<BootInspectPayload> payload) {
  BootInspectPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<BootExecutePayload> payload) {
  BootExecutePayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

void post_payload(
    const HWND window,
    const UINT message,
    std::unique_ptr<WinReDiagnosticPayload> payload) {
  WinReDiagnosticPayload* const raw = payload.release();
  if (PostMessageW(
          window,
          message,
          0,
          reinterpret_cast<LPARAM>(raw)) == FALSE) {
    delete raw;
  }
}

std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied =
      GetWindowTextW(control, result.data(), length + 1);
  result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
  return result;
}

CloneRoute selected_clone_route(const AppState& state) {
  switch (SendMessageW(state.clone_rescue_mode, CB_GETCURSEL, 0, 0)) {
    case 1:
      return CloneRoute::rescue;
    case 2:
      return CloneRoute::mbr_to_gpt;
    case 3:
      return CloneRoute::shrink;
    default:
      return CloneRoute::exact;
  }
}

std::optional<ytec::imageformat::TsumugiCreateVerificationMode>
selected_image_create_verification_mode(const AppState& state) {
  switch (SendMessageW(
      state.image_verification_mode, CB_GETCURSEL, 0, 0)) {
    case 0:
      return ytec::imageformat::TsumugiCreateVerificationMode::complete;
    case 1:
      return ytec::imageformat::TsumugiCreateVerificationMode::fast;
    default:
      return std::nullopt;
  }
}

std::wstring_view image_create_verification_mode_name(
    const ytec::imageformat::TsumugiCreateVerificationMode mode) noexcept {
  switch (mode) {
    case ytec::imageformat::TsumugiCreateVerificationMode::complete:
      return L"完全（推奨）";
    case ytec::imageformat::TsumugiCreateVerificationMode::fast:
      return L"高速（完成後の追加全走査のみ省略）";
    default:
      return L"未対応";
  }
}

bool rescue_clone_selected(const AppState& state) {
  return selected_clone_route(state) == CloneRoute::rescue;
}

void set_control_font(const HWND control, const HFONT font) {
  SendMessageW(
      control,
      WM_SETFONT,
      reinterpret_cast<WPARAM>(font),
      TRUE);
}

void set_control_visible(const HWND control, const bool visible) {
  ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void set_control_enabled(const HWND control, const bool enabled) {
  EnableWindow(control, enabled ? TRUE : FALSE);
}

HWND create_control(
    const AppState& state,
    const wchar_t* const class_name,
    const wchar_t* const text,
    const DWORD style,
    const int id) {
  return CreateWindowExW(
      0,
      class_name,
      text,
      WS_CHILD | WS_VISIBLE | style,
      0,
      0,
      10,
      10,
      state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr),
      nullptr);
}

void draw_text(
    HDC dc,
    const std::wstring_view text,
    RECT bounds,
    const HFONT font,
    const COLORREF color,
    const UINT format) {
  const HGDIOBJ old_font = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(
      dc,
      text.data(),
      static_cast<int>(text.size()),
      &bounds,
      format);
  SelectObject(dc, old_font);
}

void fill_rounded_rect(
    HDC dc,
    const RECT& bounds,
    const COLORREF fill,
    const COLORREF border) {
  const HBRUSH brush = CreateSolidBrush(fill);
  const HPEN pen = CreatePen(PS_SOLID, 1, border);
  const HGDIOBJ old_brush = SelectObject(dc, brush);
  const HGDIOBJ old_pen = SelectObject(dc, pen);
  RoundRect(
      dc,
      bounds.left,
      bounds.top,
      bounds.right,
      bounds.bottom,
      14,
      14);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  DeleteObject(pen);
  DeleteObject(brush);
}

void draw_metric_tile(
    const AppState& state,
    HDC dc,
    const RECT& bounds,
    const std::wstring_view label,
    const std::wstring_view value,
    const COLORREF accent,
    const COLORREF fill) {
  fill_rounded_rect(dc, bounds, fill, accent);
  RECT accent_bar = bounds;
  accent_bar.right = accent_bar.left + 4;
  const HBRUSH accent_brush = CreateSolidBrush(accent);
  FillRect(dc, &accent_bar, accent_brush);
  DeleteObject(accent_brush);

  RECT label_bounds{
      bounds.left + 12,
      bounds.top + 5,
      bounds.right - 8,
      bounds.top + 22};
  draw_text(
      dc,
      label,
      label_bounds,
      state.small_font,
      accent,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT value_bounds{
      bounds.left + 12,
      bounds.top + 23,
      bounds.right - 8,
      bounds.bottom - 4};
  draw_text(
      dc,
      value,
      value_bounds,
      state.small_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void draw_strands(
    HDC dc,
    const int left,
    const int top,
    const int width) {
  constexpr std::array<COLORREF, 3> kColors{
      kTsumugiBlue,
      kTsumugiPurple,
      RGB(82, 176, 132)};
  for (std::size_t index = 0; index < kColors.size(); ++index) {
    const HPEN pen = CreatePen(
        PS_SOLID,
        2,
        kColors[index]);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const int offset = static_cast<int>(index) * 7;
    POINT points[4]{
        {left, top + offset},
        {left + width / 3, top + 18 - offset},
        {left + width * 2 / 3, top - 8 + offset},
        {left + width, top + 8 + offset},
    };
    PolyBezier(dc, points, 4);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }
}

RECT content_rect(const AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  return RECT{
      260L,
      94L,
      (std::max)(client.right - 28L, 800L),
      client.bottom - 24L};
}

void draw_stepper(
    const AppState& state,
    HDC dc,
    const std::array<std::wstring_view, 4>& labels,
    const int active_step) {
  const RECT content = content_rect(state);
  const int left = content.left + 16;
  const int right = content.right - 16;
  const int y = 120;
  const int step_width = (right - left) / 4;
  const HPEN line_pen = CreatePen(PS_SOLID, 2, kBorder);
  const HGDIOBJ old_pen = SelectObject(dc, line_pen);
  MoveToEx(dc, left + step_width / 2, y, nullptr);
  LineTo(dc, right - step_width / 2, y);
  SelectObject(dc, old_pen);
  DeleteObject(line_pen);

  for (int index = 0; index < 4; ++index) {
    const int center = left + step_width * index + step_width / 2;
    const bool reached = index + 1 <= active_step;
    const HBRUSH brush = CreateSolidBrush(reached ? kTsumugiBlue : kCard);
    const HPEN pen = CreatePen(PS_SOLID, 2, reached ? kTsumugiBlue : kBorder);
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ step_old_pen = SelectObject(dc, pen);
    Ellipse(dc, center - 11, y - 11, center + 11, y + 11);
    SelectObject(dc, step_old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT number_bounds{center - 10, y - 9, center + 11, y + 10};
    draw_text(
        dc,
        std::to_wstring(index + 1),
        number_bounds,
        state.small_font,
        reached ? RGB(255, 255, 255) : kMuted,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT label_bounds{
        center - step_width / 2,
        y + 17,
        center + step_width / 2,
        y + 40};
    draw_text(
        dc,
        labels[static_cast<std::size_t>(index)],
        label_bounds,
        state.small_font,
        reached ? kInk : kMuted,
        DT_CENTER | DT_TOP | DT_SINGLELINE);
  }
}

void show_page_controls(AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  const bool compact_height = client.bottom < 600;
  const bool clone = state.page == Page::clone;
  const bool image_create = state.page == Page::image_create;
  const bool image_restore = state.page == Page::image_restore;
  const bool repair = state.page == Page::boot_repair;
  const bool disk = state.page == Page::disk_diagnostics;

  for (const HWND control :
       {state.clone_path, state.clone_browse, state.clone_check}) {
    set_control_visible(control, clone);
  }
  set_control_visible(state.clone_output, clone && !compact_height);
  set_control_visible(
      state.clone_rescue_mode,
      clone && !state.operation_busy && !state.clone_execution_ready &&
          !state.clone_progress_active);
  const bool show_clone_confirmation =
      clone && state.clone_execution_ready && !state.clone_progress_active;
  for (const HWND control :
       {state.clone_acknowledge, state.clone_token, state.clone_execute}) {
    set_control_visible(control, show_clone_confirmation);
  }
  set_control_visible(
      state.clone_cancel, clone && state.clone_progress_active);
  set_control_visible(
      state.clone_pause,
      clone && state.clone_progress_active &&
          state.clone_pause_controller != nullptr);
  for (const HWND control :
       {state.image_source,
        state.image_destination,
        state.image_browse,
        state.image_rescue_mode,
        state.image_verification_mode,
        state.image_encryption,
        state.image_review}) {
    set_control_visible(control, image_create);
  }
  set_control_visible(
      state.image_output, image_create && !compact_height);
  const bool show_image_confirmation =
      image_create && state.image_execution_ready &&
      !state.image_progress_active;
  // Source-to-file image creation is non-destructive to the Source and does
  // not use the uppercase destructive-target confirmation token. The review
  // itself is the immutable confirmation boundary.
  set_control_visible(state.image_token, false);
  set_control_visible(state.image_execute, show_image_confirmation);
  set_control_visible(
      state.image_cancel,
      image_create && state.image_progress_active);
  set_control_visible(
      state.image_pause,
      image_create && state.image_progress_active &&
          state.image_pause_controller != nullptr);
  for (const HWND control :
       {state.restore_path,
        state.restore_browse,
        state.restore_verify,
        state.restore_source_partition,
        state.restore_target,
         state.restore_target_partition,
         state.restore_review}) {
    set_control_visible(control, image_restore);
  }
  set_control_visible(
      state.restore_output, image_restore && !compact_height);
  set_control_visible(
      state.restore_target_partition,
      image_restore &&
          selected_restore_source_partition(state).has_value());
  const bool show_restore_confirmation =
      image_restore && state.restore_execution_ready &&
      !state.restore_progress_active;
  set_control_visible(state.restore_token, show_restore_confirmation);
  set_control_visible(state.restore_execute, show_restore_confirmation);
  set_control_visible(
      state.restore_cancel,
      image_restore && state.restore_progress_active);
  set_control_visible(
      state.restore_pause,
      image_restore && state.restore_progress_active &&
          state.restore_pause_controller != nullptr);
  for (const HWND control :
       {state.repair_disk, state.repair_inspect, state.repair_output}) {
    set_control_visible(control, repair);
  }
  for (const HWND control :
       {state.windows_root,
        state.system_root,
        state.firmware,
        state.winre_diagnostic,
        state.repair_acknowledge}) {
    set_control_visible(control, false);
  }
  const bool show_boot_confirmation =
      repair &&
      (state.inspected_system_partition_creation.has_value() ||
       (state.inspected_boot_plan.has_value() &&
        state.inspected_boot_choices.has_value() &&
        state.inspected_boot_execution.has_value() &&
        !state.inspected_boot_selections.empty())) &&
      !state.boot_execution_active;
  SetWindowTextW(
      state.repair_execute,
      state.inspected_system_partition_creation.has_value()
          ? L"システム領域を作成"
          : L"起動修復を実行");
  set_control_visible(state.repair_token, show_boot_confirmation);
  set_control_visible(state.repair_execute, show_boot_confirmation);
  set_control_visible(state.repair_cancel, show_boot_confirmation);
  for (const HWND control :
       {state.refresh, state.disk_list, state.disk_details}) {
    set_control_visible(control, disk);
  }
}

void layout_controls(AppState& state) {
  RECT client{};
  GetClientRect(state.window, &client);
  const RECT content = content_rect(state);
  const int width = content.right - content.left;
  const int bottom = content.bottom;
  const auto repair_actions =
      ytec::winpeapp::build_winpe_repair_action_layout(client.right);
  const auto automatic_repair =
      ytec::winpeapp::build_winpe_automatic_boot_repair_layout(
          client.right, client.bottom);
  const auto image_create =
      ytec::winpeapp::build_winpe_image_create_layout(
          client.right, client.bottom);
  const auto image_restore =
      ytec::winpeapp::build_winpe_image_restore_layout(
          client.right, client.bottom);
  const auto place = [](const HWND control,
                        const ytec::winpeapp::UiRectangle& bounds) {
    SetWindowPos(
        control,
        nullptr,
        bounds.left,
        bounds.top,
        bounds.width(),
        bounds.height(),
        SWP_NOZORDER);
  };

  SetWindowPos(
      state.nav_clone,
      nullptr,
      16,
      kSidebarNavTop,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.nav_repair,
      nullptr,
      16,
      kSidebarNavTop + 3 * kSidebarNavPitch,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.nav_disk,
      nullptr,
      16,
      kSidebarNavTop + 4 * kSidebarNavPitch,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.nav_image_create,
      nullptr,
      16,
      kSidebarNavTop + kSidebarNavPitch,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.nav_image_restore,
      nullptr,
      16,
      kSidebarNavTop + 2 * kSidebarNavPitch,
      198,
      kSidebarNavHeight,
      SWP_NOZORDER);

  const int clone_field_width = (std::max)(250, (width - 212) / 2);
  SetWindowPos(
      state.clone_path,
      nullptr,
      content.left + 22,
      kClonePathControlTop,
      clone_field_width,
      240,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_browse,
      nullptr,
      content.left + 34 + clone_field_width,
      kClonePathControlTop,
      clone_field_width,
      240,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_check,
      nullptr,
      content.right - 174,
      kClonePathControlTop,
      142,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_rescue_mode,
      nullptr,
      content.left + 22,
      326,
      width - 44,
      120,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_acknowledge,
      nullptr,
      content.left + 22,
      340,
      width - 44,
      24,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_token,
      nullptr,
      content.left + 22,
      375,
      (std::max)(width - 246, 300),
      30,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_execute,
      nullptr,
      content.right - 196,
      374,
      164,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_pause,
      nullptr,
      content.right - 330,
      420,
      144,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_cancel,
      nullptr,
      content.right - 176,
      420,
      144,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.clone_output,
      nullptr,
      content.left + 22,
      client.bottom < 600 ? 476 : 514,
      width - 44,
      client.bottom < 600
          ? (std::max)(bottom - 476, 1)
          : (std::max)(bottom - 536, 84),
      SWP_NOZORDER);

  SetWindowPos(
      state.image_source,
      nullptr,
      image_create.source.left,
      image_create.source.top,
      image_create.source.width(),
      240,
      SWP_NOZORDER);
  place(state.image_destination, image_create.destination);
  place(state.image_browse, image_create.browse);
  place(state.image_rescue_mode, image_create.rescue_mode);
  place(state.image_verification_mode, image_create.verification_mode);
  place(state.image_encryption, image_create.encryption);
  place(state.image_review, image_create.review);
  place(state.image_token, image_create.confirmation_token);
  place(state.image_execute, image_create.execute);
  place(state.image_pause, image_create.pause);
  place(state.image_cancel, image_create.cancel);
  place(state.image_output, image_create.output);

  place(state.restore_path, image_restore.image_path);
  place(state.restore_browse, image_restore.browse);
  place(state.restore_verify, image_restore.verify);
  place(
      state.restore_source_partition, image_restore.source_partition);
  place(state.restore_target, image_restore.target);
  place(
      state.restore_target_partition, image_restore.target_partition);
  place(state.restore_review, image_restore.review);
  place(state.restore_token, image_restore.confirmation_token);
  place(state.restore_execute, image_restore.execute);
  place(state.restore_pause, image_restore.pause);
  place(state.restore_cancel, image_restore.cancel);
  place(state.restore_output, image_restore.output);

  SetWindowPos(
      state.refresh,
      nullptr,
      content.right - 144,
      108,
      124,
      32,
      SWP_NOZORDER);
  const int list_width = (std::max)(330, width * 38 / 100);
  SetWindowPos(
      state.disk_list,
      nullptr,
      content.left + 16,
      194,
      list_width,
      (std::max)(bottom - 210, 270),
      SWP_NOZORDER);
  SetWindowPos(
      state.disk_details,
      nullptr,
      content.left + 30 + list_width,
      194,
      width - list_width - 46,
      (std::max)(bottom - 210, 270),
      SWP_NOZORDER);

  const int field_top = 199;
  const int field_gap = 14;
  const int field_width = (width - 44 - field_gap * 3) / 4;
  SetWindowPos(
      state.repair_disk,
      nullptr,
      content.left + 22,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.windows_root,
      nullptr,
      content.left + 22 + field_width + field_gap,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.system_root,
      nullptr,
      content.left + 22 + (field_width + field_gap) * 2,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.firmware,
      nullptr,
      content.left + 22 + (field_width + field_gap) * 3,
      field_top,
      field_width,
      300,
      SWP_NOZORDER);
  SetWindowPos(
      state.winre_diagnostic,
      nullptr,
      repair_actions.winre_diagnostic_button.left,
      repair_actions.winre_diagnostic_button.top,
      repair_actions.winre_diagnostic_button.width(),
      repair_actions.winre_diagnostic_button.height(),
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_inspect,
      nullptr,
      repair_actions.boot_inspect_button.left,
      repair_actions.boot_inspect_button.top,
      repair_actions.boot_inspect_button.width(),
      repair_actions.boot_inspect_button.height(),
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_acknowledge,
      nullptr,
      content.left + 22,
      kRepairAcknowledgeTop,
      width - 44,
      kRepairAcknowledgeHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_token,
      nullptr,
      content.left + 22,
      kRepairTokenControlTop,
      width - 250,
      kRepairTokenControlHeight,
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_execute,
      nullptr,
      content.right - 212,
      kRepairTokenControlTop,
      180,
      32,
      SWP_NOZORDER);
  SetWindowPos(
      state.repair_output,
      nullptr,
      content.left + 22,
      kRepairOutputTop,
      width - 44,
      (std::max)(bottom - (kRepairOutputTop + 22), 110),
      SWP_NOZORDER);

  SetWindowPos(
      state.repair_disk,
      nullptr,
      automatic_repair.target_disk.left,
      automatic_repair.target_disk.top,
      automatic_repair.target_disk.width(),
      300,
      SWP_NOZORDER);
  place(state.repair_inspect, automatic_repair.inspect);
  place(state.repair_token, automatic_repair.confirmation_token);
  place(state.repair_execute, automatic_repair.execute);
  place(state.repair_cancel, automatic_repair.cancel_review);
  place(state.repair_output, automatic_repair.output);

  InvalidateRect(state.window, nullptr, TRUE);
  show_page_controls(state);
  static_cast<void>(client);
}

void update_disk_details(AppState& state) {
  const LRESULT selected =
      SendMessageW(state.disk_list, LB_GETCURSEL, 0, 0);
  if (selected == LB_ERR ||
      static_cast<std::size_t>(selected) >= state.dashboard.disks.size()) {
    SetWindowTextW(
        state.disk_details,
        L"ディスクを選ぶと、読み取り専用の詳細を表示します。");
    return;
  }
  SetWindowTextW(
      state.disk_details,
      state.dashboard.disks[static_cast<std::size_t>(selected)]
          .details.c_str());
}

void invalidate_boot_review(AppState& state) {
  state.inspected_boot_plan.reset();
  state.inspected_system_partition_creation.reset();
  state.inspected_boot_choices.reset();
  state.inspected_boot_execution.reset();
  state.inspected_efi_delete_plan.reset();
  state.inspected_boot_selections.clear();
  state.boot_execution_active = false;
  state.boot_confirmation_token.clear();
  state.repair_step = 1;
  SendMessageW(
      state.repair_acknowledge,
      BM_SETCHECK,
      BST_UNCHECKED,
      0);
  SetWindowTextW(state.repair_token, L"");
  SetWindowTextW(
      state.repair_output,
      L"対象ディスクを一つ選び、「自動解析」を押してください。"
      L"\r\nWindows、システム領域、MBR/GPT、起動方式を読み取り専用で確認します。"
      L"\r\nこの段階ではBCDやディスクを変更しません。");
  show_page_controls(state);
  InvalidateRect(state.window, nullptr, FALSE);
}

std::optional<std::uint32_t> selected_disk_number(const HWND combo) {
  const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (selected == CB_ERR) {
    return std::nullopt;
  }
  const LRESULT data =
      SendMessageW(combo, CB_GETITEMDATA, selected, 0);
  if (data == CB_ERR || data < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(data);
}

bool select_combo_disk_number(
    const HWND combo,
    const std::uint32_t disk_number) {
  const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
  if (count == CB_ERR) {
    return false;
  }
  for (LRESULT index = 0; index < count; ++index) {
    const LRESULT data = SendMessageW(
        combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
    if (data != CB_ERR && data >= 0 &&
        static_cast<std::uint64_t>(data) == disk_number) {
      return SendMessageW(combo, CB_SETCURSEL, index, 0) != CB_ERR;
    }
  }
  return false;
}

ytec::bootrepair::BcdBootFirmware selected_firmware(
    const AppState& state) {
  const LRESULT selected =
      SendMessageW(state.firmware, CB_GETCURSEL, 0, 0);
  return selected == 1
      ? ytec::bootrepair::BcdBootFirmware::bios
      : ytec::bootrepair::BcdBootFirmware::uefi;
}

bool is_drive_root(const std::wstring& root) {
  return root.size() == 3 && std::iswalpha(root[0]) != 0 &&
         root[1] == L':' && root[2] == L'\\';
}

bool boot_repair_text_equal(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool boot_repair_partition_equal(
    const ytec::diskmodel::PartitionInfo& left,
    const ytec::diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      boot_repair_text_equal(left.type, right.type) &&
      boot_repair_text_equal(left.identifier, right.identifier) &&
      boot_repair_text_equal(left.name, right.name) &&
      left.bootable == right.bootable;
}

bool boot_repair_disk_equal(
    const ytec::diskmodel::DiskInfo& left,
    const ytec::diskmodel::DiskInfo& right) {
  return left.disk_number == right.disk_number &&
      boot_repair_text_equal(left.device_path, right.device_path) &&
      boot_repair_text_equal(
          left.device_instance_id, right.device_instance_id) &&
      boot_repair_text_equal(left.model, right.model) &&
      left.size_bytes == right.size_bytes &&
      left.sector_count == right.sector_count &&
      left.logical_sector_size == right.logical_sector_size &&
      left.physical_sector_size == right.physical_sector_size &&
      boot_repair_text_equal(left.bus_type, right.bus_type) &&
      left.serial_suffix == right.serial_suffix &&
      left.partition_style == right.partition_style &&
      left.offline == right.offline && left.read_only == right.read_only &&
      left.removable == right.removable &&
      left.is_system_disk == right.is_system_disk &&
      left.partitions.size() == right.partitions.size() &&
      std::equal(
          left.partitions.begin(), left.partitions.end(),
          right.partitions.begin(), boot_repair_partition_equal);
}

ytec::clonecore::Error boot_repair_winre_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

ytec::clonecore::Status revalidate_current_pc_nvram_target(
    const ytec::clonecore::StableDiskIdentity& requested_identity,
    const ytec::diskmodel::PartitionInfo& requested_esp,
    const ytec::bootrepair::AutomaticBootRepairPlan& reviewed_plan,
    const ytec::diskmodel::PartitionInfo& reviewed_esp,
    ytec::diskmodel::IDiskInventoryProvider& inventory) {
  if (!boot_repair_partition_equal(requested_esp, reviewed_esp)) {
    return ytec::clonecore::Status::failure(boot_repair_winre_error(
        ytec::clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"UEFI NVRAMレビュー済みESP",
        L"NVRAM要求のESPがレビュー済みシステム領域と一致しません"));
  }
  auto disk = ytec::diskmodel::reidentify_read_only_physical_disk(
      requested_identity, inventory);
  if (!disk) {
    return ytec::clonecore::Status::failure(disk.error());
  }
  const auto& observed = disk.value().observed;
  if (!boot_repair_disk_equal(reviewed_plan.selected_disk, observed)) {
    return ytec::clonecore::Status::failure(boot_repair_winre_error(
        ytec::clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"UEFI NVRAM対象ディスク再照合",
        L"対象ディスクの安定識別、安全属性、または完全レイアウトが変化しました"));
  }
  if (observed.is_system_disk || !observed.offline.has_value() ||
      !observed.read_only.has_value() || !observed.removable.has_value() ||
      observed.offline.value() || observed.read_only.value() ||
      observed.removable.value() || observed.partition_style !=
          ytec::diskmodel::PartitionStyle::gpt ||
      observed.logical_sector_size != 512U) {
    return ytec::clonecore::Status::failure(boot_repair_winre_error(
        ytec::clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"UEFI NVRAM対象安全属性",
        L"オンライン、書込み可能、固定、非起動環境、512バイトGPTを再確認できません"));
  }
  const auto matches = static_cast<std::size_t>(std::count_if(
      observed.partitions.begin(), observed.partitions.end(),
      [&](const auto& partition) {
        return boot_repair_partition_equal(partition, reviewed_esp);
      }));
  if (matches != 1U) {
    return ytec::clonecore::Status::failure(boot_repair_winre_error(
        ytec::clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"UEFI NVRAM対象ESP再照合",
        L"レビュー済みGPT ESPを完全一致で一意に再確認できません"));
  }
  return ytec::clonecore::success_status();
}

ytec::clonecore::Result<std::wstring> query_boot_repair_system_directory() {
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return ytec::clonecore::Result<std::wstring>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"WinRE登録用System32パス取得",
            GetLastError()));
  }
  return ytec::clonecore::Result<std::wstring>::success(
      std::wstring(buffer.data(), length));
}

ytec::clonecore::Result<
    ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution>
observe_and_bind_boot_repair_winre_images(
    ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution execution) {
  std::vector<ytec::winpeapp::
      WinPeAutomaticBootRepairWinReImageBinding> bindings;
  for (const auto& action : execution.winre_actions_in_boot_priority) {
    if (action.disposition != ytec::bootrepair::
            AutomaticWinReRepairDisposition::
                register_verified_windows_image) {
      continue;
    }
    auto identity = ytec::bootrepair::
        observe_winre_registration_image_with_windows_apis(
            action.candidate_directory + L"\\Winre.wim");
    if (!identity) {
      return ytec::clonecore::Result<
          ytec::winpeapp::
              WinPeReviewedAutomaticBootRepairExecution>::failure(
          identity.error());
    }
    bindings.push_back({
        .windows_partition_number = action.windows_partition_number,
        .identity = identity.take_value(),
    });
  }
  return ytec::winpeapp::
      bind_reviewed_automatic_boot_repair_winre_images(
          std::move(execution), bindings);
}

ytec::clonecore::Result<
    ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution>
carry_reviewed_boot_repair_winre_images(
    ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution execution,
    const ytec::winpeapp::WinPeReviewedAutomaticBootRepairExecution&
        reviewed) {
  std::vector<ytec::winpeapp::
      WinPeAutomaticBootRepairWinReImageBinding> bindings;
  for (const auto& action : reviewed.winre_actions_in_boot_priority) {
    if (action.reviewed_candidate.has_value()) {
      bindings.push_back({
          .windows_partition_number = action.windows_partition_number,
          .identity = *action.reviewed_candidate,
      });
    }
  }
  return ytec::winpeapp::
      bind_reviewed_automatic_boot_repair_winre_images(
          std::move(execution), bindings);
}

class WinPeAutomaticBootRepairWinReTargetGuard final
    : public ytec::bootrepair::IWinReRegistrationTargetGuard {
 public:
  WinPeAutomaticBootRepairWinReTargetGuard(
      const ytec::bootrepair::AutomaticBootRepairPlan& plan,
      const std::uint32_t windows_partition_number,
      ytec::diskmodel::IDiskInventoryProvider& inventory)
      : expected_identity_(plan.selected_identity),
        expected_disk_(plan.selected_disk),
        inventory_(inventory) {
    const auto found = std::find_if(
        plan.windows_installations.begin(),
        plan.windows_installations.end(),
        [&](const auto& windows) {
          return windows.partition.number == windows_partition_number;
        });
    if (found != plan.windows_installations.end()) {
      expected_windows_ = *found;
      initialized_ = true;
    }
  }

  ytec::clonecore::Status revalidate_target() override {
    if (!initialized_) {
      return ytec::clonecore::Status::failure(boot_repair_winre_error(
          ytec::clonecore::ErrorCode::invalid_data,
          ERROR_NOT_FOUND,
          L"WinRE登録対象Windows",
          L"レビュー済みWindows区画を対象計画から一意に取得できません"));
    }
    auto disk = ytec::diskmodel::reidentify_read_only_physical_disk(
        expected_identity_, inventory_);
    if (!disk) {
      return ytec::clonecore::Status::failure(disk.error());
    }
    if (!boot_repair_disk_equal(expected_disk_, disk.value().observed)) {
      return ytec::clonecore::Status::failure(boot_repair_winre_error(
          ytec::clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"WinRE登録対象レイアウト再照合",
          L"安定識別済みディスクの番号、属性、または完全パーティション配置が変化しました"));
    }
    const auto& observed_disk = disk.value().observed;
    if (observed_disk.is_system_disk ||
        !observed_disk.read_only.has_value() ||
        observed_disk.read_only.value() ||
        !observed_disk.removable.has_value() ||
        observed_disk.removable.value()) {
      return ytec::clonecore::Status::failure(boot_repair_winre_error(
          ytec::clonecore::ErrorCode::unsupported_layout,
          ERROR_ACCESS_DENIED,
          L"WinRE登録対象安全属性",
          L"実行中システムではないwriteableな固定ディスクとして再確認できません"));
    }

    auto all_volumes =
        ytec::bootrepair::enumerate_windows_boot_volumes_read_only();
    if (!all_volumes) {
      return ytec::clonecore::Status::failure(all_volumes.error());
    }
    auto volumes = ytec::bootrepair::
        filter_boot_repair_volumes_for_selected_disk(
            disk.value().observed, all_volumes.take_value());
    if (!volumes) {
      return ytec::clonecore::Status::failure(volumes.error());
    }
    const auto matches = static_cast<std::size_t>(std::count_if(
        volumes.value().begin(), volumes.value().end(),
        [&](const auto& volume) {
          return boot_repair_text_equal(
                     volume.volume_name,
                     expected_windows_.volume.volume_name) &&
              volume.location.disk_number == expected_disk_.disk_number &&
              volume.location.starting_offset ==
                  expected_windows_.partition.offset_bytes &&
              volume.location.extent_length ==
                  expected_windows_.partition.size_bytes &&
              boot_repair_text_equal(volume.location.file_system, L"NTFS") &&
              volume.mount_points.size() == 1U &&
              expected_windows_.volume.mount_points.size() == 1U &&
              boot_repair_text_equal(
                  volume.mount_points.front(),
                  expected_windows_.volume.mount_points.front());
        }));
    if (matches != 1U) {
      return ytec::clonecore::Status::failure(boot_repair_winre_error(
          ytec::clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"WinRE登録Windows Volume再照合",
          L"候補パスのドライブ文字、Volume GUID、区画範囲、NTFSを一意に再確認できません"));
    }
    return ytec::clonecore::success_status();
  }

 private:
  ytec::clonecore::StableDiskIdentity expected_identity_;
  ytec::diskmodel::DiskInfo expected_disk_;
  ytec::bootrepair::DiscoveredWindowsInstallation expected_windows_;
  ytec::diskmodel::IDiskInventoryProvider& inventory_;
  bool initialized_{};
};

std::wstring root_file_system(const std::wstring& root) {
  if (!is_drive_root(root)) {
    return {};
  }
  std::array<wchar_t, 64> file_system{};
  if (GetVolumeInformationW(
          root.c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size())) == FALSE) {
    return {};
  }
  return file_system.data();
}

bool root_contains_windows(const std::wstring& root) {
  const std::wstring system_hive =
      root + L"Windows\\System32\\Config\\SYSTEM";
  const DWORD attributes = GetFileAttributesW(system_hive.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                        FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

std::vector<std::wstring> enumerate_local_roots() {
  std::vector<std::wstring> roots;
  const DWORD mask = GetLogicalDrives();
  for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
    if (letter == L'X') {
      continue;
    }
    const DWORD bit = 1UL << static_cast<unsigned int>(letter - L'A');
    if ((mask & bit) == 0) {
      continue;
    }
    std::wstring root{letter, L':', L'\\'};
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE ||
        type == DRIVE_RAMDISK) {
      roots.push_back(std::move(root));
    }
  }
  return roots;
}

void select_combo_text(const HWND combo, const std::wstring& text) {
  const LRESULT index =
      SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(text.c_str()));
  if (index != CB_ERR) {
    SendMessageW(combo, CB_SETCURSEL, index, 0);
  } else {
    SetWindowTextW(combo, text.c_str());
  }
}

void select_default_roots(AppState& state) {
  std::wstring windows_root;
  for (const auto& root : state.roots) {
    if (root_contains_windows(root)) {
      windows_root = root;
      break;
    }
  }
  if (windows_root.empty() && !state.roots.empty()) {
    windows_root = state.roots.front();
  }
  if (!windows_root.empty()) {
    select_combo_text(state.windows_root, windows_root);
  }

  const bool uefi =
      selected_firmware(state) == ytec::bootrepair::BcdBootFirmware::uefi;
  std::wstring system_root;
  for (const auto& root : state.roots) {
    const std::wstring file_system = root_file_system(root);
    if ((uefi && _wcsicmp(file_system.c_str(), L"FAT32") == 0 &&
         _wcsicmp(root.c_str(), windows_root.c_str()) != 0) ||
        (!uefi && _wcsicmp(file_system.c_str(), L"NTFS") == 0)) {
      system_root = root;
      break;
    }
  }
  if (system_root.empty()) {
    system_root = kAutoSystemPartitionLabel;
  }
  if (!system_root.empty()) {
    select_combo_text(state.system_root, system_root);
  }
}

void populate_root_controls(AppState& state) {
  state.roots = enumerate_local_roots();
  SendMessageW(state.windows_root, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.system_root, CB_RESETCONTENT, 0, 0);
  SendMessageW(
      state.system_root,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(kAutoSystemPartitionLabel));
  for (const auto& root : state.roots) {
    SendMessageW(
        state.windows_root,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(root.c_str()));
    SendMessageW(
        state.system_root,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(root.c_str()));
  }
  select_default_roots(state);
}

void populate_disk_controls(AppState& state) {
  SendMessageW(state.disk_list, LB_RESETCONTENT, 0, 0);
  SendMessageW(state.repair_disk, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.clone_path, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.clone_browse, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.image_source, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.restore_target, CB_RESETCONTENT, 0, 0);
  for (const auto& disk : state.dashboard.disks) {
    SendMessageW(
        state.disk_list,
        LB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(disk.list_label.c_str()));
    if (!disk.system_disk) {
      const LRESULT source_index = SendMessageW(
          state.clone_path,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(disk.list_label.c_str()));
      if (source_index != CB_ERR && source_index != CB_ERRSPACE) {
        SendMessageW(
            state.clone_path,
            CB_SETITEMDATA,
            static_cast<WPARAM>(source_index),
            static_cast<LPARAM>(disk.disk_number));
      }
      const LRESULT image_source_index = SendMessageW(
          state.image_source,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(disk.list_label.c_str()));
      if (image_source_index != CB_ERR &&
          image_source_index != CB_ERRSPACE) {
        SendMessageW(
            state.image_source,
            CB_SETITEMDATA,
            static_cast<WPARAM>(image_source_index),
            static_cast<LPARAM>(disk.disk_number));
      }
    }
    if (disk.selectable_as_target) {
      const LRESULT index = SendMessageW(
          state.repair_disk,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(disk.list_label.c_str()));
      if (index != CB_ERR && index != CB_ERRSPACE) {
        SendMessageW(
            state.repair_disk,
            CB_SETITEMDATA,
            static_cast<WPARAM>(index),
            static_cast<LPARAM>(disk.disk_number));
      }
      const LRESULT target_index = SendMessageW(
          state.clone_browse,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(disk.list_label.c_str()));
      if (target_index != CB_ERR && target_index != CB_ERRSPACE) {
        SendMessageW(
            state.clone_browse,
            CB_SETITEMDATA,
            static_cast<WPARAM>(target_index),
            static_cast<LPARAM>(disk.disk_number));
      }
      const LRESULT restore_target_index = SendMessageW(
          state.restore_target,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(disk.list_label.c_str()));
      if (restore_target_index != CB_ERR &&
          restore_target_index != CB_ERRSPACE) {
        SendMessageW(
            state.restore_target,
            CB_SETITEMDATA,
            static_cast<WPARAM>(restore_target_index),
            static_cast<LPARAM>(disk.disk_number));
      }
    }
  }
  if (!state.dashboard.disks.empty()) {
    SendMessageW(state.disk_list, LB_SETCURSEL, 0, 0);
  }
  if (SendMessageW(state.repair_disk, CB_GETCOUNT, 0, 0) > 0) {
    SendMessageW(state.repair_disk, CB_SETCURSEL, 0, 0);
  }
  if (SendMessageW(state.clone_path, CB_GETCOUNT, 0, 0) > 0) {
    SendMessageW(state.clone_path, CB_SETCURSEL, 0, 0);
  }
  if (SendMessageW(state.clone_browse, CB_GETCOUNT, 0, 0) > 0) {
    SendMessageW(state.clone_browse, CB_SETCURSEL, 0, 0);
  }
  if (SendMessageW(state.image_source, CB_GETCOUNT, 0, 0) > 0) {
    SendMessageW(state.image_source, CB_SETCURSEL, 0, 0);
  }
  if (state.image_resume_requested &&
      state.reviewed_image_resume_observation.has_value() &&
      state.reviewed_image_resume_observation->source.has_value()) {
    static_cast<void>(select_combo_disk_number(
        state.image_source,
        state.reviewed_image_resume_observation->source->disk_number));
  }
  if (SendMessageW(state.restore_target, CB_GETCOUNT, 0, 0) > 0) {
    SendMessageW(state.restore_target, CB_SETCURSEL, 0, 0);
  }
  if (state.verified_restore_image.has_value()) {
    populate_restore_target_partition_candidates(state);
  }
  update_disk_details(state);
}

ytec::winpeapp::WinPeImageCreateUiView current_image_create_view(
    const AppState& state) {
  const bool inventory_ready =
      state.inventory.has_value() &&
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready;
  const bool cancellation_requested =
      state.image_cancellation != nullptr &&
      state.image_cancellation->load(std::memory_order_relaxed);
  return ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = inventory_ready,
      .idle = !state.operation_busy && !state.inventory_busy,
      .source_selected =
          selected_disk_number(state.image_source).has_value(),
      .destination_entered = !control_text(state.image_destination).empty(),
      .verification_mode_selected =
          selected_image_create_verification_mode(state).has_value(),
      .reviewed = state.image_execution_ready &&
          state.reviewed_image_source.has_value(),
      .progress_active = state.image_progress_active,
      .confirmation_text = control_text(state.image_token),
      .cancellation_allowed = state.image_progress.cancellation_allowed,
      .cancellation_requested = cancellation_requested,
  });
}

ytec::winpeapp::WinPeImageRestoreUiView current_image_restore_view(
    const AppState& state) {
  const bool inventory_ready =
      state.inventory.has_value() &&
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready;
  const auto selected_target = selected_disk_number(state.restore_target);
  const bool individual_selected =
      selected_restore_source_partition(state).has_value();
  const auto individual_target =
      current_restore_individual_partition_selection(state);
  const bool reviewed_target_matches =
      state.reviewed_restore_target.has_value() &&
      selected_target.has_value() &&
      state.reviewed_restore_target->disk_number == selected_target.value() &&
      state.reviewed_restore_individual_partition == individual_target;
  const bool cancellation_requested =
      state.restore_cancellation != nullptr &&
      state.restore_cancellation->load(std::memory_order_relaxed);
  return ytec::winpeapp::build_winpe_image_restore_ui_view({
      .inventory_ready = inventory_ready,
      .idle = !state.operation_busy && !state.inventory_busy,
      .image_path_entered = !control_text(state.restore_path).empty(),
      .image_verified = state.verified_restore_image.has_value(),
      .individual_partition_selected = individual_selected,
      .individual_target_selected = individual_target.has_value(),
      .target_selected = selected_target.has_value(),
      .target_reviewed = state.restore_execution_ready &&
          state.reviewed_restore_target.has_value(),
      .reviewed_target_matches_selection = reviewed_target_matches,
      .image_storage_disk_known = state.verified_restore_image.has_value(),
      .image_is_on_selected_target =
          ytec::winpeapp::is_image_stored_on_restore_target(
              state.verified_restore_image.has_value()
                  ? std::optional<std::uint32_t>(
                        state.verified_restore_image->storage_disk_number)
                  : std::nullopt,
              selected_target),
      .active_rescue_media_resolver_available = true,
      .progress_active = state.restore_progress_active,
      .confirmation_text = control_text(state.restore_token),
      .cancellation_allowed = state.restore_progress.cancellation_allowed,
      .cancellation_requested = cancellation_requested,
  });
}

ytec::winpeapp::WinPeAutomaticBootRepairUiView current_boot_repair_view(
    const AppState& state) {
  const bool inventory_ready =
      state.inventory.has_value() &&
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready;
  return ytec::winpeapp::build_winpe_automatic_boot_repair_ui_view({
      .inventory_ready = inventory_ready,
      .idle = !state.operation_busy && !state.inventory_busy,
      .target_selected =
          selected_disk_number(state.repair_disk).has_value(),
      .reviewed = state.inspected_system_partition_creation.has_value() ||
          (state.inspected_boot_plan.has_value() &&
           state.inspected_boot_choices.has_value() &&
           state.inspected_boot_execution.has_value() &&
           !state.inspected_boot_selections.empty()),
      .execution_active = state.boot_execution_active,
      .confirmation_text = control_text(state.repair_token),
  });
}

void update_action_state(AppState& state) {
  const bool inventory_ready =
      state.inventory.has_value() &&
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready;
  const bool rescue_inventory_ready =
      state.inventory.has_value() && state.inventory->issues.empty() &&
      (state.dashboard.readiness ==
           ytec::winpeapp::DashboardReadiness::ready ||
       state.dashboard.readiness ==
           ytec::winpeapp::DashboardReadiness::warning);
  const bool idle = !state.operation_busy && !state.inventory_busy;
  const auto source_disk = selected_disk_number(state.clone_path);
  const auto target_disk = selected_disk_number(state.clone_browse);
  const bool direct_selection_ready =
      source_disk.has_value() && target_disk.has_value() &&
      source_disk != target_disk;
  set_control_enabled(
      state.clone_check,
      idle &&
          (rescue_clone_selected(state) ? rescue_inventory_ready
                                        : inventory_ready) &&
          state.dashboard.direct_clone_available &&
          direct_selection_ready);
  set_control_enabled(state.clone_path, idle);
  set_control_enabled(state.clone_browse, idle);
  set_control_enabled(state.clone_rescue_mode, idle);
  const bool clone_acknowledged =
      SendMessageW(state.clone_acknowledge, BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  const bool clone_token_matches =
      !state.clone_confirmation_token.empty() &&
      control_text(state.clone_token) == state.clone_confirmation_token;
  set_control_enabled(
      state.clone_acknowledge,
      idle && state.clone_execution_ready);
  set_control_enabled(
      state.clone_token,
      idle && state.clone_execution_ready);
  set_control_enabled(
      state.clone_execute,
      idle && state.clone_execution_ready && clone_acknowledged &&
          clone_token_matches);
  const bool cancellation_already_requested =
      state.clone_cancellation != nullptr &&
      state.clone_cancellation->load(std::memory_order_relaxed);
  set_control_enabled(
      state.clone_cancel,
      state.operation_busy && state.clone_progress_active &&
          state.clone_progress.cancellation_allowed &&
          !cancellation_already_requested);

  const auto image_view = current_image_create_view(state);
  set_control_enabled(state.image_source, image_view.source_enabled);
  set_control_enabled(
      state.image_destination, image_view.destination_enabled);
  set_control_enabled(state.image_browse, image_view.browse_enabled);
  set_control_enabled(
      state.image_rescue_mode, image_view.rescue_mode_enabled);
  set_control_enabled(
      state.image_verification_mode,
      image_view.verification_mode_enabled);
  set_control_enabled(
      state.image_encryption, image_view.encryption_enabled);
  set_control_enabled(state.image_review, image_view.review_enabled);
  set_control_enabled(
      state.image_token, image_view.confirmation_enabled);
  set_control_enabled(state.image_execute, image_view.execute_enabled);
  set_control_enabled(state.image_cancel, image_view.cancel_enabled);
  const auto restore_view = current_image_restore_view(state);
  set_control_enabled(
      state.restore_path, restore_view.image_path_enabled);
  set_control_enabled(state.restore_browse, restore_view.browse_enabled);
  set_control_enabled(state.restore_verify, restore_view.verify_enabled);
  set_control_enabled(
      state.restore_source_partition,
      restore_view.source_partition_enabled &&
          (!state.verified_restore_image.has_value() ||
           state.verified_restore_image->mode !=
               ytec::imageformat::TsumugiManifestMode::shrink));
  set_control_enabled(state.restore_target, restore_view.target_enabled);
  set_control_enabled(
      state.restore_target_partition,
      restore_view.target_partition_enabled &&
          (!state.verified_restore_image.has_value() ||
           state.verified_restore_image->mode !=
               ytec::imageformat::TsumugiManifestMode::shrink));
  set_control_enabled(state.restore_review, restore_view.review_enabled);
  set_control_enabled(
      state.restore_token, restore_view.confirmation_enabled);
  set_control_enabled(state.restore_execute, restore_view.execute_enabled);
  set_control_enabled(state.restore_cancel, restore_view.cancel_enabled);
  set_control_enabled(
      state.refresh,
      !state.inventory_busy && !state.operation_busy);
  const auto boot_view = current_boot_repair_view(state);
  set_control_enabled(state.repair_disk, boot_view.target_enabled);
  set_control_enabled(state.repair_inspect, boot_view.inspect_enabled);
  set_control_enabled(
      state.repair_token, boot_view.confirmation_enabled);
  set_control_enabled(state.repair_execute, boot_view.execute_enabled);
  set_control_enabled(
      state.repair_cancel, boot_view.cancel_review_enabled);
  for (const HWND control :
       {state.windows_root,
        state.system_root,
        state.firmware,
        state.winre_diagnostic,
        state.repair_acknowledge}) {
    set_control_enabled(control, false);
  }
  update_manual_pause_button(
      state.clone_pause,
      state.page == Page::clone && state.clone_progress_active,
      state.clone_pause_controller);
  update_manual_pause_button(
      state.image_pause,
      state.page == Page::image_create && state.image_progress_active,
      state.image_pause_controller);
  update_manual_pause_button(
      state.restore_pause,
      state.page == Page::image_restore && state.restore_progress_active &&
          state.restore_write_active,
      state.restore_pause_controller);
}

void invalidate_clone_review(
    AppState& state,
    const std::wstring_view message) {
  state.clone_execution_ready = false;
  state.reviewed_clone_plan.reset();
  state.reviewed_rescue_clone_plan.reset();
  state.reviewed_mbr2gpt_clone_plan.reset();
  state.reviewed_direct_shrink_clone_plan.reset();
  state.clone_progress_active = false;
  state.clone_confirmation_token.clear();
  state.clone_cancellation.reset();
  state.clone_pause_controller.reset();
  state.clone_elapsed = std::chrono::milliseconds{};
  state.clone_progress = ytec::clonecore::DiskOperationProgress{};
  SendMessageW(
      state.clone_acknowledge,
      BM_SETCHECK,
      BST_UNCHECKED,
      0);
  SetWindowTextW(state.clone_token, L"");
  SetWindowTextW(state.clone_cancel, L"安全に取消");
  SetWindowTextW(state.clone_pause, L"一時停止不可");
  if (!message.empty()) {
    SetWindowTextW(state.clone_output, std::wstring(message).c_str());
  }
  show_page_controls(state);
}

void invalidate_image_review(
    AppState& state,
    const std::wstring_view message) {
  state.image_step = 1;
  state.reviewed_image_source.reset();
  state.reviewed_image_partition_selection.reset();
  state.reviewed_image_path.clear();
  state.reviewed_image_replace_existing = false;
  state.reviewed_image_encrypted = false;
  state.reviewed_image_rescue_mode = false;
  state.reviewed_image_verification_mode =
      ytec::imageformat::TsumugiCreateVerificationMode::complete;
  state.reviewed_image_password.reset();
  state.image_execution_ready = false;
  state.image_progress_active = false;
  state.image_progress = ytec::clonecore::DiskOperationProgress{};
  state.image_elapsed = std::chrono::milliseconds{};
  state.image_cancellation.reset();
  state.image_pause_controller.reset();
  SetWindowTextW(state.image_token, L"");
  SetWindowTextW(state.image_cancel, L"安全に取消");
  SetWindowTextW(state.image_pause, L"一時停止不可");
  if (!message.empty()) {
    SetWindowTextW(state.image_output, std::wstring(message).c_str());
  }
  show_page_controls(state);
}

void invalidate_restore_target_review(
    AppState& state,
    const std::wstring_view message) {
  state.reviewed_restore_target.reset();
  state.reviewed_restore_target_identity.reset();
  state.reviewed_restore_target_layout_hash.reset();
  state.reviewed_restore_individual_partition.reset();
  state.reviewed_shrink_restore_work.reset();
  state.reviewed_shrink_restore_layout.reset();
  state.reviewed_shrink_original_source_target.reset();
  state.restore_execution_ready = false;
  state.restore_step = state.verified_restore_image.has_value() ? 2 : 1;
  SetWindowTextW(state.restore_token, L"");
  if (!message.empty()) {
    SetWindowTextW(state.restore_output, std::wstring(message).c_str());
  }
  show_page_controls(state);
}

void invalidate_restore_image_review(
    AppState& state,
    const std::wstring_view message) {
  state.verified_restore_image.reset();
  populate_restore_source_partition_candidates(state);
  state.reviewed_restore_image_password.reset();
  state.restore_progress_active = false;
  state.restore_write_active = false;
  state.restore_progress = ytec::clonecore::DiskOperationProgress{};
  state.restore_elapsed = std::chrono::milliseconds{};
  state.restore_cancellation.reset();
  state.restore_pause_controller.reset();
  SetWindowTextW(state.restore_cancel, L"安全に取消");
  SetWindowTextW(state.restore_pause, L"一時停止不可");
  invalidate_restore_target_review(state, message);
  state.restore_step = 1;
}

void start_inventory(AppState& state) {
#if defined(YTEC_UI_ACCEPTANCE_BUILD)
  state.inventory_busy = false;
  state.inventory.reset();
  state.dashboard = ytec::winpeapp::DashboardView{
      .readiness = ytec::winpeapp::DashboardReadiness::blocked,
      .headline = L"UI受入モード",
      .guidance =
          L"物理ディスク列挙と製品I/Oを無効化しています。"};
  SetWindowTextW(
      state.disk_details,
      L"UI受入モードでは物理ディスクへアクセスしません。");
  EnableWindow(state.refresh, FALSE);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  return;
#else
  if (state.inventory_busy || state.operation_busy) {
    return;
  }
  state.clone_step = 1;
  invalidate_clone_review(
      state,
      L"ディスク情報が更新されました。"
      L"\r\nコピー元とコピー先を選び、読取り専用の安全確認を行ってください。");
  invalidate_image_review(
      state,
      L"ディスク情報が更新されました。\r\n"
      L"コピー元と .tsumugi 保存先を選び、内容確認を行ってください。");
  invalidate_restore_target_review(
      state,
      state.verified_restore_image.has_value()
          ? L"ディスク情報が更新されました。復元先を選び直して、"
            L"消去内容を再確認してください。"
          : L"ディスク情報が更新されました。まず .tsumugi を完全検証してください。");
  invalidate_boot_review(state);
  state.inventory_busy = true;
  state.inventory.reset();
  state.dashboard = ytec::winpeapp::DashboardView{
      .readiness = ytec::winpeapp::DashboardReadiness::scanning,
      .headline = L"物理ディスクを確認しています",
      .guidance = L"読み取り専用で情報を集めています。しばらくお待ちください。"};
  SetWindowTextW(
      state.disk_details,
      L"ディスク情報を読み取り専用で確認しています…");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND window = state.window;
  std::thread([window]() {
    auto payload = std::make_unique<InventoryPayload>();
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto result = provider->enumerate();
    if (result) {
      payload->inventory = result.take_value();
    } else {
      payload->error = format_error(
          result.error(), &payload->error_presentation);
    }
    post_payload(window, kInventoryCompleteMessage, std::move(payload));
  }).detach();
#endif
}

void start_clone_check(AppState& state) {
  const auto source = selected_disk_number(state.clone_path);
  const auto target = selected_disk_number(state.clone_browse);
  const CloneRoute route = selected_clone_route(state);
  if (state.operation_busy || !source.has_value() || !target.has_value() ||
      source == target) {
    return;
  }

  std::optional<
      ytec::winpeapp::WinPeOfflineNtfsProductPlanningRequest>
      shrink_request;
  if (route == CloneRoute::shrink) {
    if (!current_process_is_elevated() ||
        !current_environment_is_verified_winpe()) {
      invalidate_clone_review(
          state,
          L"縮小移行は、管理者権限で起動した正式なWinPE環境だけで実行できます。\r\n"
          L"通常WindowsやUI受入ビルドから物理ディスクを変更しません。");
      update_action_state(state);
      return;
    }
    const auto source_disk = find_inventory_disk(state, *source);
    const auto target_disk = find_inventory_disk(state, *target);
    if (!source_disk.has_value() || !target_disk.has_value()) {
      invalidate_clone_review(
          state,
          L"選択したコピー元またはコピー先を現在の一覧で確認できません。再読込みしてください。");
      update_action_state(state);
      return;
    }
    auto operation_id = make_operation_id(L"WinPE直接縮小クローン操作ID");
    const std::string created_utc = current_utc_timestamp();
    if (!operation_id || created_utc.empty()) {
      invalidate_clone_review(
          state,
          !operation_id
              ? format_error(operation_id.error(), nullptr)
              : L"縮小移行の解析時刻をUTCで確定できません。");
      update_action_state(state);
      return;
    }
    const int consent = MessageBoxW(
        state.window,
        L"縮小移行の読取り解析を開始すると、選択したコピー元をOSレベルでread-onlyに固定します。\n\n"
        L"この設定は完了・取消・失敗後も自動解除しません。コピー元へは書き込みません。\n"
        L"コピー先は、パーティション選択・余剰容量設定と大文字OKが完了するまで変更しません。\n\n"
        L"コピー元をread-onlyに固定して解析しますか？",
        L"WinPE 縮小移行のコピー元保護",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (consent != IDYES) {
      invalidate_clone_review(
          state,
          L"縮小移行の読取り解析を取り消しました。ディスクは変更していません。");
      update_action_state(state);
      return;
    }
    shrink_request =
        ytec::winpeapp::WinPeOfflineNtfsProductPlanningRequest{
            .administrator = true,
            .winpe_environment_verified = true,
            .reviewed_source = *source_disk,
            .reviewed_target = *target_disk,
            .operation_id = operation_id.take_value(),
            .surplus_allocation = ytec::migrationcore::
                ShrinkSurplusAllocation::automatic_proportional,
            .analysis_created_utc = created_utc,
            .app_version = YTEC_PROJECT_VERSION,
        };
  }

  invalidate_clone_review(state, L"");
  state.operation_busy = true;
  state.clone_step = 2;
  SetWindowTextW(
      state.clone_output,
      route == CloneRoute::rescue
          ? L"救出クローン対象を読取り専用で再列挙し、\r\n"
            L"安定識別、容量、セクター、安全属性、起動中レスキューUSBとの不一致を確認しています。\r\n"
            L"この確認ではどのディスクも変更しません。"
          : route == CloneRoute::mbr_to_gpt
          ? L"MBR→GPT（別ディスク）の対象を読取り専用で再列挙し、\r\n"
            L"安定識別、同容量以上、1～3基本プライマリ領域、一意なActive領域、健康状態を確認しています。\r\n"
            L"この確認ではどのディスクも変更しません。"
          : route == CloneRoute::shrink
          ? L"コピー元をOS-level read-onlyに固定し、コピー元・コピー先を安定再識別しています。\r\n"
            L"GPT/512B/Windows/BitLocker/健康状態と、NTFS再構築／未対応FSの元サイズRAW可否を自動解析します。\r\n"
            L"この段階ではコピー先を変更しません。コピー元のread-only設定は安全のため維持します。"
          : L"コピー元とコピー先を読取り専用で再列挙し、\r\n"
            L"安定識別、ディスク形式、容量、セクター、安全属性を確認しています。\r\n"
            L"この確認ではどのディスクも変更しません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([
      window,
      source = *source,
      target = *target,
      route,
      shrink_request = std::move(shrink_request)]() mutable {
    if (route == CloneRoute::shrink) {
      auto inspection_payload =
          std::make_unique<CloneShrinkInspectionPayload>();
      if (!shrink_request.has_value()) {
        inspection_payload->output =
            L"縮小移行の製品解析requestが失われたため、コピー先を変更せず停止しました。";
      } else {
        inspection_payload->request = std::move(*shrink_request);
        auto inspection = ytec::winpeapp::
            inspect_winpe_offline_ntfs_direct_shrink_with_windows_apis(
                inspection_payload->request);
        if (inspection) {
          inspection_payload->inspection = inspection.take_value();
          inspection_payload->output =
              L"コピー元のread-only固定と自動解析が完了しました。"
              L"コピーするパーティションと余剰容量の使い方を選択してください。";
        } else {
          inspection_payload->output = format_error(
              inspection.error(), &inspection_payload->error_presentation);
          inspection_payload->output +=
              L"\r\n\r\n読取り解析の途中でコピー元のread-only固定が"
              L"完了している場合、安全のため自動解除しません。"
              L"コピー先は変更していません。";
        }
      }
      post_payload(
          window,
          kCloneShrinkInspectionCompleteMessage,
          std::move(inspection_payload));
      return;
    }
    auto payload = std::make_unique<CloneCheckPayload>();
    payload->route = route;
    auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
    if (route == CloneRoute::rescue) {
      auto plan = ytec::winpeapp::prepare_rescue_clone_operation(
          source,
          target,
          *provider,
          [](const ytec::clonecore::StableDiskIdentity& expected_target) {
            return ytec::winpeapp::
                query_active_rescue_media_target_with_windows_apis(
                    expected_target, {});
          });
      payload->success = plan.has_value();
      payload->execution_ready = plan.has_value();
      payload->confirmation_token = payload->success ? L"OK" : L"";
      if (plan) {
        payload->output = format_rescue_clone_plan(plan.value());
        payload->reviewed_rescue_plan = plan.take_value();
      } else {
        payload->output = format_error(
            plan.error(), &payload->error_presentation);
      }
      if (payload->output.empty()) {
        payload->output = payload->success
            ? L"救出クローンの安全確認が完了しました。"
            : L"救出クローンの安全確認に失敗しました。";
      }
      post_payload(window, kCloneCheckCompleteMessage, std::move(payload));
      return;
    }
    if (route == CloneRoute::mbr_to_gpt) {
      auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
          source, target, *provider);
      payload->success = plan.has_value();
      payload->execution_ready = plan.has_value();
      payload->confirmation_token = payload->success ? L"OK" : L"";
      if (plan) {
        payload->output = format_mbr2gpt_clone_plan(plan.value());
        payload->reviewed_mbr2gpt_plan = plan.take_value();
      } else {
        payload->output = format_error(
            plan.error(), &payload->error_presentation);
      }
      if (payload->output.empty()) {
        payload->output = payload->success
            ? L"MBR→GPT（別ディスク）の安全確認が完了しました。"
            : L"MBR→GPT（別ディスク）の安全確認に失敗しました。";
      }
      post_payload(window, kCloneCheckCompleteMessage, std::move(payload));
      return;
    }
    auto plan = ytec::winpeapp::prepare_direct_clone_operation(
        source, target, *provider);
    payload->success = plan.has_value();
    payload->execution_ready = plan.has_value();
    payload->confirmation_token = payload->success ? L"OK" : L"";
    if (plan) {
      payload->output = format_direct_clone_plan(plan.value());
      payload->reviewed_plan = plan.take_value();
    } else {
      payload->output = format_error(
          plan.error(), &payload->error_presentation);
    }
    if (payload->output.empty()) {
      payload->output = payload->success
          ? L"直接クローンの安全確認が完了しました。"
          : L"直接クローンの安全確認に失敗しました。";
    }
    post_payload(window, kCloneCheckCompleteMessage, std::move(payload));
  }).detach();
}

void start_clone_shrink_plan_after_review(
    AppState& state,
    ytec::winpeapp::WinPeOfflineNtfsProductPlanningRequest request,
    ytec::winpeapp::WinPeOfflineNtfsPartitionReviewBinding review_binding) {
  state.operation_busy = true;
  state.clone_step = 2;
  SetWindowTextW(
      state.clone_output,
      L"選択内容を固定し、コピー元・コピー先のstable identity、GPT配置、"
      L"source raw snapshot、NTFS使用量、Windows/Boot/WinRE役割を再解析しています。\r\n"
      L"最初の解析と完全一致した場合だけ、不変の縮小移行計画を作成します。"
      L"コピー先はまだ変更しません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([
      window,
      request = std::move(request),
      review_binding = std::move(review_binding)]() mutable {
    auto payload = std::make_unique<CloneCheckPayload>();
    payload->route = CloneRoute::shrink;
    auto plan = ytec::winpeapp::
        plan_winpe_offline_ntfs_direct_shrink_after_review_with_windows_apis(
            request,
            review_binding);
    payload->success = plan.has_value();
    payload->execution_ready = plan.has_value();
    payload->confirmation_token = plan ? L"OK" : L"";
    if (plan) {
      auto retained = std::make_shared<const
          ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan>(
          plan.take_value());
      payload->output = format_winpe_offline_direct_shrink_plan(*retained);
      payload->reviewed_direct_shrink_plan = std::move(retained);
    } else {
      payload->output = format_error(
          plan.error(), &payload->error_presentation);
      payload->output +=
          L"\r\n\r\n再解析が一致しない、4Kn、BitLocker未復号、"
          L"未対応FSを元サイズRAWとして安全に保持できない、"
          L"staging容量不足、または安全に解釈できない構成ではコピー先を変更せず停止します。"
          L"コピー元はread-onlyのままです。";
    }
    post_payload(window, kCloneCheckCompleteMessage, std::move(payload));
  }).detach();
}

void start_clone_execute(AppState& state) {
  const bool acknowledged =
      SendMessageW(state.clone_acknowledge, BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  const CloneRoute route = state.reviewed_direct_shrink_clone_plan != nullptr
      ? CloneRoute::shrink
      : state.reviewed_mbr2gpt_clone_plan.has_value()
      ? CloneRoute::mbr_to_gpt
      : state.reviewed_rescue_clone_plan.has_value()
      ? CloneRoute::rescue
      : CloneRoute::exact;
  const std::size_t reviewed_plan_count =
      (state.reviewed_clone_plan.has_value() ? 1U : 0U) +
      (state.reviewed_rescue_clone_plan.has_value() ? 1U : 0U) +
      (state.reviewed_mbr2gpt_clone_plan.has_value() ? 1U : 0U) +
      (state.reviewed_direct_shrink_clone_plan != nullptr ? 1U : 0U);
  if (state.operation_busy || !state.clone_execution_ready ||
      reviewed_plan_count != 1U ||
      !acknowledged || control_text(state.clone_token) != L"OK") {
    return;
  }
  if (!confirm_long_operation_power(
          state.window,
           route == CloneRoute::rescue
               ? L"救出クローン"
               : route == CloneRoute::mbr_to_gpt
               ? L"MBRからGPTへの別ディスク移行"
               : route == CloneRoute::shrink
               ? L"WinPE 縮小移行クローン"
               : L"ドライブのクローン")) {
    return;
  }

  const auto reviewed_plan = state.reviewed_clone_plan;
  const auto reviewed_rescue_plan = state.reviewed_rescue_clone_plan;
  const auto reviewed_mbr2gpt_plan = state.reviewed_mbr2gpt_clone_plan;
  const auto reviewed_direct_shrink_plan =
      state.reviewed_direct_shrink_clone_plan;
  auto resume_slot_admission = [&]() -> ytec::clonecore::Result<
      ytec::operationcore::OperationPlan> {
    if (route == CloneRoute::shrink) {
      return ytec::clonecore::Result<
          ytec::operationcore::OperationPlan>::success(
          reviewed_direct_shrink_plan->operation_plan());
    }
    if (route == CloneRoute::rescue) {
      const auto& reviewed = reviewed_rescue_plan.value();
      return ytec::clonecore::Result<
          ytec::operationcore::OperationPlan>::success({
          .operation_id = reviewed.operation_id,
          .kind = ytec::operationcore::OperationKind::rescue_clone,
          .environment = ytec::operationcore::OperationEnvironment::winpe,
          .source = reviewed.expected_source,
          .target = reviewed.expected_target,
          .expected_work_bytes = reviewed.expected_source.size_bytes,
          .immutable_payload_hash = reviewed.expected_source_layout_hash,
      });
    }

    auto operation_id = make_operation_id(
        route == CloneRoute::mbr_to_gpt
            ? L"WinPE MBR→GPT clone resume-slot admission ID"
            : L"WinPE direct clone resume-slot admission ID");
    if (!operation_id) {
      return ytec::clonecore::Result<
          ytec::operationcore::OperationPlan>::failure(
          operation_id.error());
    }
    const auto& reviewed = route == CloneRoute::mbr_to_gpt
        ? reviewed_mbr2gpt_plan->clone
        : reviewed_plan.value();
    const std::array<std::wstring, 5U> owned_fields{
        route == CloneRoute::mbr_to_gpt ? L"mbr-to-gpt" : L"exact-clone",
        reviewed.source_bus_type,
        reviewed.target_bus_type,
        std::to_wstring(reviewed.source_partition_count),
        std::to_wstring(reviewed.target_partition_count),
    };
    const std::array<std::wstring_view, 5U> fields{
        owned_fields[0], owned_fields[1], owned_fields[2], owned_fields[3],
        owned_fields[4],
    };
    const std::array<ytec::operationcore::Sha256Digest, 1U> mbr2gpt_digest{
        route == CloneRoute::mbr_to_gpt
            ? reviewed_mbr2gpt_plan->review_binding_digest
            : ytec::operationcore::Sha256Digest{},
    };
    return ytec::winpeapp::make_winpe_resume_slot_admission_plan(
        operation_id.take_value(),
        ytec::operationcore::OperationKind::clone,
        reviewed.expected_source,
        reviewed.expected_target,
        reviewed.expected_source.size_bytes,
        fields,
        route == CloneRoute::mbr_to_gpt
            ? std::span<const ytec::operationcore::Sha256Digest>(mbr2gpt_digest)
            : std::span<const ytec::operationcore::Sha256Digest>{});
  }();
  if (!guard_new_product_operation_start(
          state.window, state.clone_output, resume_slot_admission)) {
    return;
  }
  const std::uint64_t completion_power_binding =
      ytec::winpeapp::take_winpe_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  const std::wstring completion_power_target =
      route == CloneRoute::shrink
      ? format_completion_disk_target(
            reviewed_direct_shrink_plan->target_plan().expected_target())
      : route == CloneRoute::mbr_to_gpt
      ? format_completion_disk_target(
            reviewed_mbr2gpt_plan->clone.expected_target)
      : route == CloneRoute::rescue
      ? format_completion_disk_target(
            reviewed_rescue_plan->expected_target)
      : format_completion_disk_target(reviewed_plan->expected_target);

  state.operation_busy = true;
  state.clone_execution_ready = false;
  state.clone_progress_active = true;
  state.clone_step = 4;
  state.clone_elapsed = std::chrono::milliseconds{};
  state.clone_progress = ytec::clonecore::DiskOperationProgress{
      .stage = ytec::clonecore::DiskOperationStage::planning,
      .cancellation_allowed = true,
  };
  state.clone_cancellation = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = state.clone_cancellation;
  state.clone_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.clone_pause_controller;
  SetWindowTextW(
      state.clone_output,
      route == CloneRoute::rescue
          ? L"救出実行直前にコピー元・コピー先と起動中レスキューUSBをもう一度再識別します。\r\n"
            L"一致した場合だけコピー元をread-onlyに固定し、コピー先をofflineにしてRAW救出します。\r\n"
            L"縮小・形式変換・起動修復は行いません。結果は常に「一部欠損の可能性あり」として扱います。"
          : route == CloneRoute::mbr_to_gpt
          ? L"実行直前にコピー元・コピー先とMicrosoft署名済み変換ツールをもう一度確認します。\r\n"
            L"一致した場合だけコピー元をread-onlyに固定し、別のコピー先へ検証済みMBRクローンを作成します。\r\n"
            L"コピー先だけをGPTへ変換し、ESP・MSR・UEFI BCDを検証して最後にofflineへ戻します。"
          : route == CloneRoute::shrink
          ? L"実行直前にコピー元・コピー先をstable identityで再識別します。\r\n"
            L"コピー元をOS-level read-onlyのまま保持し、各NTFS capture／exact RAW直前にsourceを再検証します。\r\n"
            L"コピー先は非起動stagingから構築し、全書込み読戻し・Boot/WinRE証拠後にGPTをcommit lastしてoffline保持します。"
          : L"実行直前にコピー元とコピー先をもう一度再識別します。\r\n"
            L"一致した場合だけ、コピー元を読取り専用で保持し、コピー先だけに書き込みます。\r\n"
            L"書込み開始後の取消・失敗では、コピー先をオフラインのまま保護する場合があります。");
  SetWindowTextW(state.clone_cancel, L"安全に取消");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([
      window,
      reviewed_plan,
      reviewed_rescue_plan,
      reviewed_mbr2gpt_plan,
      reviewed_direct_shrink_plan,
      route,
      completion_power_binding,
      completion_power_target,
      cancellation,
      pause_controller]() {
    auto payload = std::make_unique<CloneExecutePayload>();
    payload->route = route;
    payload->completion_power_target = completion_power_target;
    ThreadSleepPrevention sleep_prevention;
    if (!sleep_prevention.active()) {
      payload->success = false;
      payload->output = route == CloneRoute::shrink
          ? L"縮小移行中の自動スリープ防止を確立できないため、"
            L"コピー先を変更せずに停止しました。"
            L"コピー元はOS-level read-onlyのままです。"
          : L"クローン中の自動スリープ防止を確立できないため、"
            L"ディスクを変更せずに停止しました。";
      pause_controller->mark_completed();
      post_payload(
          window, kCloneExecuteCompleteMessage, std::move(payload));
      return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto last_posted = std::make_shared<std::chrono::steady_clock::time_point>(
        started - std::chrono::seconds(1));
    ytec::clonecore::DiskOperationCallbacks callbacks{
        .progress =
            [window, started, last_posted](
                const ytec::clonecore::DiskOperationProgress& progress) {
              const auto now = std::chrono::steady_clock::now();
              if (now - *last_posted < std::chrono::milliseconds(100)) {
                return;
              }
              *last_posted = now;
              auto update = std::make_unique<CloneProgressPayload>();
              update->progress = progress;
              update->elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - started);
              post_payload(window, kCloneProgressMessage, std::move(update));
            },
        .cancellation_requested =
            [cancellation]() {
              return cancellation->load(std::memory_order_relaxed);
        },
    };
    callbacks = ytec::clonecore::bind_manual_pause_controller(
        std::move(callbacks), pause_controller);
    if (route == CloneRoute::rescue) {
      auto dependencies =
          ytec::winpeapp::make_rescue_clone_windows_dependencies();
      auto execution = ytec::winpeapp::execute_rescue_clone_operation(
          reviewed_rescue_plan.value(),
          true,
          L"OK",
          dependencies,
          std::move(callbacks));
      payload->success = execution.has_value() &&
          execution.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          execution.value().rescue.has_value();
      if (payload->success) {
        payload->output = ytec::winpeapp::format_rescue_clone_product_result(
            execution.value().rescue.value());
      } else if (!execution) {
        payload->output = format_error(
            execution.error(), &payload->error_presentation);
      } else if (execution.value().lifecycle.error.has_value()) {
        payload->output = format_error(
            execution.value().lifecycle.error.value(),
            &payload->error_presentation);
      } else {
        payload->output =
            L"救出クローンOperationを完了できませんでした。";
      }
    } else if (route == CloneRoute::mbr_to_gpt) {
      auto migration_service =
          ytec::winpeapp::make_windows_mbr2gpt_direct_execution_service();
      auto execution = ytec::winpeapp::execute_mbr2gpt_direct_operation(
          reviewed_mbr2gpt_plan.value(),
          true,
          L"OK",
          *migration_service,
          std::move(callbacks));
      payload->success = execution.has_value() &&
          mbr2gpt_clone_completion_verified(execution.value());
      if (execution && payload->success) {
        payload->output = format_mbr2gpt_clone_result(execution.value());
      } else if (execution) {
        payload->output =
            L"MBR→GPT移行の必須証拠が一つでも不完全なため、"
            L"完了扱いにしません。コピー先はofflineのままです。";
      } else {
        payload->output = format_error(
            execution.error(), &payload->error_presentation);
      }
    } else if (route == CloneRoute::shrink) {
      ytec::winpeapp::WinPeOfflineNtfsDirectShrinkExecutionOptions options{
          .confirmation = {
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
          .callbacks = callbacks,
      };
      const auto dependencies = ytec::winpeapp::
          make_winpe_offline_ntfs_direct_shrink_dependencies_with_windows_apis(
              options);
      auto execution = ytec::winpeapp::
          execute_winpe_offline_ntfs_direct_shrink_clone(
              *reviewed_direct_shrink_plan,
              options,
              dependencies);
      payload->success = execution.has_value() &&
          winpe_offline_direct_shrink_completion_verified(
              *reviewed_direct_shrink_plan, execution.value());
      if (payload->success) {
        payload->output =
            format_winpe_offline_direct_shrink_result(execution.value());
      } else if (!execution) {
        payload->output = format_error(
            execution.error(), &payload->error_presentation);
      } else if (execution.value().lifecycle.error.has_value()) {
        payload->output = format_error(
            *execution.value().lifecycle.error,
            &payload->error_presentation);
      } else {
        payload->output =
            L"WinPE縮小移行の必須証拠が一つでも不完全なため、"
            L"完了扱いにしません。コピー先はofflineのままです。";
      }
    } else {
      auto clone_service =
          ytec::winpeapp::make_windows_clone_execution_service();
      auto execution = ytec::winpeapp::execute_direct_clone_operation(
          reviewed_plan.value(),
          true,
          L"OK",
          L"",
          *clone_service,
          std::move(callbacks));
      payload->success = execution.has_value() &&
          direct_clone_completion_verified(execution.value());
      if (execution && payload->success) {
        payload->output = format_direct_clone_result(execution.value());
      } else if (execution) {
        payload->output =
            L"直接クローンの必須証拠が一つでも不完全なため、"
            L"完了扱いにしません。コピー先はofflineのままです。";
      } else {
        payload->output = format_error(
            execution.error(), &payload->error_presentation);
      }
    }
    if (payload->output.empty()) {
      payload->output = payload->success
          ? route == CloneRoute::rescue
              ? L"救出クローンの全書込み読戻し検証が完了しました。"
              : route == CloneRoute::mbr_to_gpt
              ? L"MBR→GPT別ディスク移行と最終offline確認が完了しました。"
              : route == CloneRoute::shrink
              ? L"WinPE縮小移行の全必須証拠と最終offline確認が完了しました。"
              : L"クローンと読戻し検証が完了しました。"
          : route == CloneRoute::rescue
              ? L"救出クローンを安全に完了できませんでした。"
              : route == CloneRoute::mbr_to_gpt
              ? L"MBR→GPT別ディスク移行を安全に完了できませんでした。"
              : route == CloneRoute::shrink
              ? L"WinPE縮小移行を安全に完了できませんでした。"
              : L"クローンを安全に完了できませんでした。";
    }
    pause_controller->mark_completed();
    const auto sleep_prevention_release = sleep_prevention.release();
    const bool partial_result = route == CloneRoute::rescue;
    payload->completion_power_proof =
        ytec::winpeapp::make_winpe_completion_power_proof(
            ytec::winpeapp::WinPeCompletionPowerOperation::clone,
            completion_operation_outcome(
                payload->success, partial_result && payload->success),
            payload->success,
            sleep_prevention_release,
            completion_power_binding);
    if (payload->success && sleep_prevention_release !=
            ytec::clonecore::SleepPreventionReleaseState::released) {
      payload->output +=
          L"\r\n\r\n自動スリープ防止の解除を確認できないため、"
          L"完了後の再起動／シャットダウンは提示しません。";
    }
    post_payload(window, kCloneExecuteCompleteMessage, std::move(payload));
  }).detach();
}

void request_clone_cancellation(AppState& state) {
  if (!state.clone_progress_active ||
      !state.clone_progress.cancellation_allowed ||
      state.clone_cancellation == nullptr) {
    return;
  }
  state.clone_cancellation->store(true, std::memory_order_relaxed);
  if (state.clone_pause_controller != nullptr) {
    static_cast<void>(state.clone_pause_controller->request_cancel());
  }
  SetWindowTextW(state.clone_cancel, L"取消要求済み");
  std::wstring output = control_text(state.clone_output);
  output +=
      L"\r\n\r\n取消要求を受け付けました。安全な境界で停止します。"
      L"\r\n最終確定中は中断せず、整合性を守ってから終了します。";
  SetWindowTextW(state.clone_output, output.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
}

std::optional<ytec::diskmodel::DiskInfo> find_inventory_disk(
    const AppState& state,
    const std::uint32_t disk_number) {
  if (!state.inventory.has_value()) {
    return std::nullopt;
  }
  const auto found = std::find_if(
      state.inventory->disks.begin(),
      state.inventory->disks.end(),
      [disk_number](const ytec::diskmodel::DiskInfo& disk) {
        return disk.disk_number == disk_number;
      });
  return found == state.inventory->disks.end()
      ? std::nullopt
      : std::optional<ytec::diskmodel::DiskInfo>(*found);
}

bool restore_manifest_partition_selected(
    const ytec::imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              ytec::imageformat::TsumugiManifestPartitionFlags::selected)) !=
      0U;
}

std::optional<std::uint32_t> selected_restore_source_partition(
    const AppState& state) {
  const LRESULT selected = SendMessageW(
      state.restore_source_partition, CB_GETCURSEL, 0, 0);
  if (selected == CB_ERR || selected == 0) {
    return std::nullopt;
  }
  const auto candidate_index = static_cast<std::size_t>(selected - 1);
  if (candidate_index >= state.restore_source_partition_candidates.size()) {
    return std::nullopt;
  }
  return state.restore_source_partition_candidates[candidate_index];
}

std::optional<ytec::imageformat::
    TsumugiPhysicalIndividualPartitionRestoreSelection>
current_restore_individual_partition_selection(const AppState& state) {
  if (!selected_restore_source_partition(state).has_value()) {
    return std::nullopt;
  }
  const LRESULT selected = SendMessageW(
      state.restore_target_partition, CB_GETCURSEL, 0, 0);
  if (selected == CB_ERR || selected < 0 ||
      static_cast<std::size_t>(selected) >=
          state.restore_target_partition_candidates.size()) {
    return std::nullopt;
  }
  return state.restore_target_partition_candidates[
      static_cast<std::size_t>(selected)];
}

void populate_restore_target_partition_candidates(AppState& state) {
  SendMessageW(state.restore_target_partition, CB_RESETCONTENT, 0, 0);
  state.restore_target_partition_candidates.clear();
  const auto source = selected_restore_source_partition(state);
  const auto selected_disk = selected_disk_number(state.restore_target);
  if (!source.has_value() || !selected_disk.has_value() ||
      !state.verified_restore_image.has_value()) {
    update_action_state(state);
    return;
  }
  const auto target = find_inventory_disk(state, selected_disk.value());
  if (!target.has_value()) {
    update_action_state(state);
    return;
  }
  auto partitions = target->partitions;
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
                 state.verified_restore_image->manifest,
                 target.value(),
                 selection)) {
      continue;
    }
    const std::wstring label =
        L"区画 " + std::to_wstring(partition.number) + L" / " +
        ytec::winpeapp::format_dashboard_capacity(partition.size_bytes) +
        L" / " +
        (partition.name.empty() ? partition.type : partition.name);
    SendMessageW(
        state.restore_target_partition,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    state.restore_target_partition_candidates.push_back(
        std::move(selection));
  }
  const auto unallocated = ytec::imageformat::
      find_tsumugi_physical_unallocated_restore_candidates_v1(
          state.verified_restore_image->manifest,
          target.value(),
          source.value());
  if (unallocated) {
    for (const auto& selection : unallocated.value()) {
      const auto& placement = std::get<ytec::imageformat::
          TsumugiPhysicalUnallocatedRestoreSelection>(selection.target);
      const std::wstring label =
          L"未割当 / offset " +
          ytec::winpeapp::format_dashboard_capacity(
              placement.target_offset) +
          L" / 新規区画 " +
          ytec::winpeapp::format_dashboard_capacity(
              placement.target_size) +
          L"（既存区画は保持）";
      SendMessageW(
          state.restore_target_partition,
          CB_ADDSTRING,
          0,
          reinterpret_cast<LPARAM>(label.c_str()));
      state.restore_target_partition_candidates.push_back(selection);
    }
  }
  if (!state.restore_target_partition_candidates.empty()) {
    SendMessageW(state.restore_target_partition, CB_SETCURSEL, 0, 0);
  }
  update_action_state(state);
}

void populate_restore_source_partition_candidates(AppState& state) {
  SendMessageW(state.restore_source_partition, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.restore_target_partition, CB_RESETCONTENT, 0, 0);
  state.restore_source_partition_candidates.clear();
  state.restore_target_partition_candidates.clear();
  if (!state.verified_restore_image.has_value()) {
    return;
  }
  SendMessageW(
      state.restore_source_partition,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"ディスク全体を復元"));
  const auto& image = state.verified_restore_image.value();
  if (image.mode == ytec::imageformat::TsumugiManifestMode::shrink) {
    SendMessageW(state.restore_source_partition, CB_SETCURSEL, 0U, 0);
    populate_restore_target_partition_candidates(state);
    return;
  }
  for (const auto& partition : image.manifest.partitions) {
    if (!restore_manifest_partition_selected(partition)) {
      continue;
    }
    const std::wstring label =
        L"画像区画 " +
        std::to_wstring(partition.source_partition_number) + L" / " +
        ytec::winpeapp::format_dashboard_capacity(partition.source_size) +
        L" を個別復元";
    SendMessageW(
        state.restore_source_partition,
        CB_ADDSTRING,
        0,
        reinterpret_cast<LPARAM>(label.c_str()));
    state.restore_source_partition_candidates.push_back(
        partition.source_table_index);
  }
  const bool partition_selection =
      (static_cast<std::uint32_t>(image.manifest.flags) &
       static_cast<std::uint32_t>(
           ytec::imageformat::TsumugiManifestFlags::partition_selection)) !=
      0U;
  SendMessageW(
      state.restore_source_partition,
      CB_SETCURSEL,
      partition_selection &&
              !state.restore_source_partition_candidates.empty()
          ? 1U
          : 0U,
      0);
  populate_restore_target_partition_candidates(state);
}

std::string current_utc_timestamp() {
  SYSTEMTIME time{};
  GetSystemTime(&time);
  std::array<char, 32> text{};
  const int length = std::snprintf(
      text.data(),
      text.size(),
      "%04u-%02u-%02uT%02u:%02u:%02uZ",
      time.wYear,
      time.wMonth,
      time.wDay,
      time.wHour,
      time.wMinute,
      time.wSecond);
  return length > 0 && static_cast<std::size_t>(length) < text.size()
      ? std::string(text.data(), static_cast<std::size_t>(length))
      : std::string{};
}

void choose_image_destination(AppState& state) {
  if (state.operation_busy || state.inventory_busy) {
    return;
  }
  std::vector<wchar_t> path(32768U, L'\0');
  const std::wstring current = control_text(state.image_destination);
  const std::wstring initial = current.empty()
      ? L"Tsumugi-backup.tsumugi"
      : current;
  if (initial.size() >= path.size()) {
    SetWindowTextW(
        state.image_output,
        L"保存先パスが長すぎるため、選択画面を開けません。");
    return;
  }
  std::copy(initial.begin(), initial.end(), path.begin());
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter =
      L"Y-TEC Tsumugi (*.tsumugi)\0*.tsumugi\0"
      L"すべてのファイル\0*.*\0\0";
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"tsumugi";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
      OFN_DONTADDTORECENT;
  if (GetSaveFileNameW(&dialog) == FALSE) {
    return;
  }
  SetWindowTextW(state.image_destination, path.data());
  SetFocus(state.image_review);
}

void start_image_review(AppState& state) {
  const auto selected = selected_disk_number(state.image_source);
  const auto verification_mode =
      selected_image_create_verification_mode(state);
  const std::wstring destination = control_text(state.image_destination);
  if (state.operation_busy || state.inventory_busy || !selected.has_value() ||
      !verification_mode.has_value() || destination.empty()) {
    return;
  }
  invalidate_image_review(state, L"");
  const auto source = find_inventory_disk(state, selected.value());
  if (!source.has_value()) {
    SetWindowTextW(
        state.image_output,
        L"作成元を現在の読み取り専用一覧で再確認できません。再読込みしてください。");
    update_action_state(state);
    return;
  }

  const bool encrypted =
      SendMessageW(state.image_encryption, BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  const bool rescue_mode =
      SendMessageW(state.image_rescue_mode, BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  const bool resume_existing = state.image_resume_requested;
  const auto resume = state.reviewed_image_resume_observation;
  if (resume_existing &&
      (!state.image_resume_platform_ready ||
       !state.image_resume_slot_present || !resume.has_value() ||
       !resume->binding.has_value() || !resume->source.has_value() ||
       !resume->continuity.has_value())) {
    SetWindowTextW(
        state.image_output,
        L"前回の通常イメージ作成を安全に再開できる完全bindingがありません。"
        L"checkpointと所有ファイルは変更していません。");
    update_action_state(state);
    return;
  }
  if (resume_existing &&
      (_wcsicmp(destination.c_str(), resume->final_path.c_str()) != 0 ||
       rescue_mode || encrypted != resume->continuity->encrypted ||
       verification_mode.value() !=
           resume->continuity->verification_mode)) {
    SetWindowTextW(
        state.image_output,
        L"再開時は前回と同じ保存先・暗号化・検証方式を選び、"
        L"通常（exact）モードのまま内容確認してください。"
        L"表示済みcheckpointと所有ファイルは変更していません。");
    update_action_state(state);
    return;
  }

  std::optional<ytec::diskmodel::ImagePartitionSelection>
      partition_selection;
  if (rescue_mode) {
    auto whole = ytec::diskmodel::normalize_image_partition_selection(
        source.value(),
        std::span<const std::uint32_t>{});
    if (whole) {
      partition_selection = whole.take_value();
    } else {
      set_error_output_and_show(
          state.window, state.image_output, whole.error());
      update_action_state(state);
      return;
    }
  } else if (resume_existing) {
    auto resumed = ytec::diskmodel::normalize_image_partition_selection(
        source.value(),
        resume->continuity->selected_partition_numbers);
    if (resumed) {
      const auto canonical = resumed.value().whole_disk
          ? std::vector<std::uint32_t>{}
          : resumed.value().selected_partition_numbers;
      if (canonical !=
          resume->continuity->selected_partition_numbers) {
        SetWindowTextW(
            state.image_output,
            L"前回のpartition選択が現在の必須Windows領域を含むcanonical bindingと一致しません。"
            L"checkpointと所有ファイルは変更していません。");
        update_action_state(state);
        return;
      }
      partition_selection = resumed.take_value();
    } else {
      set_error_output_and_show(
          state.window,
          state.image_output,
          resumed.error(),
          L"checkpointと所有ファイルは変更していません。");
      update_action_state(state);
      return;
    }
  } else {
    partition_selection = prompt_image_partition_selection(
        state.window,
        source.value());
    if (!partition_selection.has_value()) {
      SetWindowTextW(
          state.image_output,
          L"イメージ対象の選択を取り消しました。作成元と保存先は変更していません。");
      update_action_state(state);
      return;
    }
  }

  bool replace_existing = false;
  const DWORD attributes = GetFileAttributesW(destination.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
      SetWindowTextW(
          state.image_output,
          L"保存先にはフォルダーではなく .tsumugi ファイルを指定してください。");
      update_action_state(state);
      return;
    }
    const bool resume_after_publish = resume_existing &&
        (resume->object_state == ytec::operationcore::
             PersistentPeExactImageCreateObjectState::published ||
         resume->object_state == ytec::operationcore::
             PersistentPeExactImageCreateObjectState::retirement_pending);
    if (resume_existing && !resume_after_publish) {
      SetWindowTextW(
          state.image_output,
          L"作成途中の再開先に同名の完成ファイルが現れました。"
          L"別物を採用も上書きもせず、再開情報を保持して停止しました。");
      update_action_state(state);
      return;
    }
    if (!resume_existing && !rescue_mode) {
      SetWindowTextW(
          state.image_output,
          L"通常（exact）の永続再開対応作成は既存の完成ファイルを上書きしません。"
          L"存在しない別の .tsumugi 保存名を選んでください。");
      update_action_state(state);
      return;
    }
    if (!resume_existing && rescue_mode) {
      if (MessageBoxW(
              state.window,
              L"同名の完成イメージがあります。\r\n\r\n"
              L"救出用 .partial を完全検証できた後だけ、既存ファイルを失わない手順で置換します。\r\n"
              L"この保存先を内容確認へ進めますか？",
              L"既存救出イメージの置換確認",
              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
      }
      replace_existing = true;
    }
  } else {
    const DWORD path_error = GetLastError();
    if (path_error != ERROR_FILE_NOT_FOUND &&
        path_error != ERROR_PATH_NOT_FOUND) {
      set_error_output_and_show(
          state.window,
          state.image_output,
          {
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = path_error,
              .operation = L"WinPE image-create final read-only observation",
              .message = L"完成名が存在しないことを確認できません",
          });
      update_action_state(state);
      return;
    }
  }

  auto identity = ytec::diskmodel::make_stable_disk_identity(
      source.value(), source->is_system_disk);
  if (!identity) {
    set_error_output_and_show(
        state.window, state.image_output, identity.error());
    update_action_state(state);
    return;
  }
  if (resume_existing) {
    const auto same_source = ytec::clonecore::validate_stable_identity(
        *resume->source, identity.value(), L"WinPE image-create resume Source");
    if (!same_source) {
      set_error_output_and_show(
          state.window,
          state.image_output,
          same_source.error(),
          L"前回のcheckpointと所有ファイルは変更していません。");
      update_action_state(state);
      return;
    }
  } else {
    const auto destination_safe =
        ytec::imageformat::validate_windows_tsumugi_destination({
            .final_path = destination,
            .expected_source_disk = identity.value(),
            .required_available_bytes = 1U,
            .replace_existing = replace_existing,
        });
    if (!destination_safe) {
      set_error_output_and_show(
          state.window, state.image_output, destination_safe.error());
      update_action_state(state);
      return;
    }
  }

  std::shared_ptr<SecureAsciiPassword> password;
  if (encrypted) {
    auto prompt = prompt_tsumugi_password(
        state.window,
        state.body_font,
        resume_existing
            ? L"前回の .tsumugi の暗号化パスワードを再入力してください。"
              L"パスワードと鍵はcheckpointへ保存していません。"
              L"誤った値では既存prefixの認証に失敗し、何も追記しません。"
            : L"新しい .tsumugi の暗号化パスワードを2回入力してください。"
              L"回復キーはなく、紛失すると復元できません。"
              L"パスワードは作成完了または停止時にメモリから消去します。",
        resume_existing ? L"暗号化イメージ作成を再開" : L"イメージを暗号化",
        !resume_existing,
        !resume_existing,
        resume_existing ? L"このパスワードで再開" : L"このパスワードを使用");
    if (!prompt.accepted || prompt.password == nullptr) {
      SetWindowTextW(
          state.image_output,
          resume_existing
              ? L"暗号化パスワードの再入力を取り消しました。"
                L"checkpointと所有ファイルは変更していません。"
              : L"暗号化パスワードの入力を取り消しました。"
                L"作成元と保存先は変更していません。");
      update_action_state(state);
      return;
    }
    password = std::move(prompt.password);
  }

  if (resume_existing && resume->final_path.empty()) {
    SetWindowTextW(
        state.image_output,
        L"再開情報の完成名が空のため、安全に再開できません。"
        L"checkpointと所有ファイルは変更していません。");
    update_action_state(state);
    return;
  }

  state.reviewed_image_source = source;
  state.reviewed_image_partition_selection = partition_selection;
  state.reviewed_image_path = destination;
  state.reviewed_image_replace_existing = replace_existing;
  state.reviewed_image_encrypted = encrypted;
  state.reviewed_image_rescue_mode = rescue_mode;
  state.reviewed_image_verification_mode = verification_mode.value();
  state.reviewed_image_password = std::move(password);
  state.image_execution_ready = true;
  state.image_step = 3;
  SetWindowTextW(state.image_token, L"");
  std::wstring review = format_direct_image_review(
      source.value(),
      destination,
      replace_existing,
      encrypted,
      verification_mode.value(),
      rescue_mode);
  review += L"\r\nイメージ対象: " +
      format_image_partition_selection_review(*partition_selection);
  if (resume_existing) {
    auto startup = ytec::winpeapp::
        format_direct_image_create_resume_startup_review_v1(*resume);
    if (!startup) {
      set_error_output_and_show(
          state.window, state.image_output, startup.error());
      invalidate_image_review(state, L"");
      update_action_state(state);
      return;
    }
    review += L"\r\n\r\n" + startup.value();
    review += L"\r\n再開直前にsource状態・保存先・partial/journal File ID・計画を再証明し、"
              L"確認済みprefixを全量再認証してからsuffixだけを追記します。";
  }
  SetWindowTextW(state.image_output, review.c_str());
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  SetFocus(state.image_execute);
}

void start_image_execute(AppState& state) {
  if (state.operation_busy || !state.image_execution_ready ||
      !state.reviewed_image_source.has_value() ||
      !state.reviewed_image_partition_selection.has_value() ||
      state.reviewed_image_path.empty() ||
      (state.reviewed_image_encrypted &&
       state.reviewed_image_password == nullptr)) {
    return;
  }
  if (!confirm_long_operation_power(state.window, L"イメージ作成")) {
    return;
  }
  const auto source = state.reviewed_image_source.value();
  const auto partition_selection =
      state.reviewed_image_partition_selection.value();
  const std::vector<std::uint32_t> selected_partition_numbers =
      partition_selection.whole_disk
      ? std::vector<std::uint32_t>{}
      : partition_selection.selected_partition_numbers;
  const std::wstring destination = state.reviewed_image_path;
  const bool replace_existing = state.reviewed_image_replace_existing;
  const bool rescue_mode = state.reviewed_image_rescue_mode;
  const auto verification_mode =
      state.reviewed_image_verification_mode;
  const auto password = state.reviewed_image_password;
  const bool persistent_exact = !rescue_mode;
  const bool resume_existing =
      persistent_exact && state.image_resume_requested;
  const auto resume_observation = state.reviewed_image_resume_observation;
  if (persistent_exact && !state.image_resume_platform_ready) {
    SetWindowTextW(
        state.image_output,
        L"実行ファイル隣のdataと保存先を永続再開用として安全に確認できないため、"
        L"通常イメージ作成は開始しません。");
    return;
  }
  if (resume_existing &&
      (!state.image_resume_slot_present || !resume_observation.has_value() ||
       !resume_observation->binding.has_value())) {
    SetWindowTextW(
        state.image_output,
        L"表示済みの完全bindingがないため、前回の通常イメージ作成は再開しません。"
        L"checkpointと所有ファイルは変更していません。");
    return;
  }
  auto source_identity = ytec::diskmodel::make_stable_disk_identity(
      source, source.is_system_disk);
  if (!source_identity) {
    set_error_output_and_show(
        state.window, state.image_output, source_identity.error());
    return;
  }
  ytec::operationcore::OperationId new_operation_id{};
  if (!resume_existing) {
    auto admission_operation_id = make_operation_id(
        rescue_mode
            ? L"WinPE rescue image-create resume-slot admission ID"
            : L"WinPE image-create resume-slot admission ID");
    if (!admission_operation_id) {
      set_error_output_and_show(
          state.window, state.image_output, admission_operation_id.error());
      return;
    }
    new_operation_id = admission_operation_id.value();
    const std::array<std::wstring, 6U> admission_owned_fields{
        destination,
        replace_existing ? L"replace-existing" : L"create-new",
        rescue_mode ? L"rescue" : L"exact",
        verification_mode ==
                ytec::imageformat::TsumugiCreateVerificationMode::complete
            ? L"complete-verification"
            : L"fast-verification",
        state.reviewed_image_encrypted ? L"encrypted" : L"unencrypted",
        canonical_image_partition_selection_binding(partition_selection),
    };
    const std::array<std::wstring_view, 6U> admission_fields{
        admission_owned_fields[0],
        admission_owned_fields[1],
        admission_owned_fields[2],
        admission_owned_fields[3],
        admission_owned_fields[4],
        admission_owned_fields[5],
    };
    auto resume_slot_admission =
        ytec::winpeapp::make_winpe_resume_slot_admission_plan(
            new_operation_id,
            rescue_mode
                ? ytec::operationcore::OperationKind::rescue_image
                : ytec::operationcore::OperationKind::image_create,
            source_identity.value(),
            std::nullopt,
            rescue_mode ? source.size_bytes
                        : partition_selection.selected_bytes,
            admission_fields);
    if (!guard_new_product_operation_start(
            state.window, state.image_output, resume_slot_admission)) {
      return;
    }
  } else {
    const auto same_source = ytec::clonecore::validate_stable_identity(
        *resume_observation->source,
        source_identity.value(),
        L"WinPE image-create resume final Source");
    if (!same_source) {
      set_error_output_and_show(
          state.window,
          state.image_output,
          same_source.error(),
          L"checkpointと所有ファイルは変更していません。");
      return;
    }
  }
  const std::uint64_t completion_power_binding =
      ytec::winpeapp::take_winpe_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  const std::wstring completion_power_target =
      L"出力 " + destination + L" / コピー元 " +
      format_completion_disk_target(source);
  state.reviewed_image_password.reset();
  state.operation_busy = true;
  state.image_execution_ready = false;
  state.image_progress_active = true;
  state.image_step = 4;
  state.image_elapsed = std::chrono::milliseconds{};
  state.image_progress = ytec::clonecore::DiskOperationProgress{
      .stage = ytec::clonecore::DiskOperationStage::planning,
      .cancellation_allowed = true,
  };
  state.image_cancellation = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = state.image_cancellation;
  state.image_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.image_pause_controller;
  SetWindowTextW(
      state.image_output,
      rescue_mode
          ? (verification_mode ==
                     ytec::imageformat::TsumugiCreateVerificationMode::complete
                 ? L"作成元をread-onlyに固定し、救出用RAW一時領域と保存先を再識別しています。\r\n"
                   L"有限再試行と全書込み読戻し後は故障Sourceを再読込せず、\r\n"
                   L"一時領域の封印・完全検証・破棄・保存先再識別後だけ完成名へ確定します。"
                 : L"作成元をread-onlyに固定し、救出用RAW一時領域と保存先を再識別しています。\r\n"
                   L"有限再試行と各書込み読戻し後は故障Sourceを再読込せず、\r\n"
                   L"認証・Hash・最終メタデータ検証、破棄、保存先再識別後だけ完成名へ確定します。")
          : (verification_mode ==
                     ytec::imageformat::TsumugiCreateVerificationMode::complete
                 ? L"作成元をread-onlyに固定し、安定識別とレイアウトを再確認しています。\r\n"
                   L"同一物理ディスクへの保存は拒否します。隣接 .partial へ作成し、\r\n"
                   L"各書込み読戻しと完成前の追加全走査後だけ完成名へ確定します。"
                 : L"作成元をread-onlyに固定し、安定識別とレイアウトを再確認しています。\r\n"
                   L"同一物理ディスクへの保存は拒否します。隣接 .partial へ作成し、\r\n"
                   L"各書込み読戻し・認証/Hash・最終メタデータ検証後だけ完成名へ確定します。"));
  SetWindowTextW(state.image_cancel, L"安全に取消");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([
      window,
      source,
      selected_partition_numbers,
      destination,
      replace_existing,
      rescue_mode,
      verification_mode,
      password,
      persistent_exact,
      resume_existing,
      resume_observation,
      new_operation_id,
      completion_power_binding,
      completion_power_target,
      cancellation,
      pause_controller]() {
    auto payload = std::make_unique<ImageCreatePayload>();
    payload->persistent_resume_attempt = persistent_exact;
    payload->completion_power_target = completion_power_target;
    ThreadSleepPrevention sleep_prevention;
    if (!sleep_prevention.active()) {
      payload->success = false;
      payload->output =
          L"イメージ作成中の自動スリープ防止を確立できないため、"
          L"ディスクと保存先を変更せずに停止しました。";
      if (persistent_exact) {
        auto inspected = inspect_product_image_create_resume();
        payload->resume_platform_ready = inspected.has_value();
        payload->resume_slot_present = inspected &&
            inspected.value().object_state != ytec::operationcore::
                PersistentPeExactImageCreateObjectState::no_slot;
        if (payload->resume_slot_present &&
            inspected.value().object_state != ytec::operationcore::
                PersistentPeExactImageCreateObjectState::other_capability) {
          payload->resume_observation_after_operation =
              inspected.take_value();
        }
      }
      pause_controller->mark_completed();
      post_payload(
          window, kImageCreateCompleteMessage, std::move(payload));
      return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto last_posted = std::make_shared<std::chrono::steady_clock::time_point>(
        started - std::chrono::seconds(1));
    ytec::clonecore::DiskOperationCallbacks callbacks{
        .progress =
            [window, started, last_posted](
                const ytec::clonecore::DiskOperationProgress& progress) {
              const auto now = std::chrono::steady_clock::now();
              if (now - *last_posted < std::chrono::milliseconds(100)) {
                return;
              }
              *last_posted = now;
              auto update = std::make_unique<ImageCreateProgressPayload>();
              update->progress = progress;
              update->elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - started);
              post_payload(
                  window, kImageCreateProgressMessage, std::move(update));
            },
        .cancellation_requested =
            [cancellation]() {
              return cancellation->load(std::memory_order_relaxed);
        },
    };
    callbacks = ytec::clonecore::bind_manual_pause_controller(
        std::move(callbacks), pause_controller);
    const ytec::winpeapp::DirectImageCreateRequest request{
            .selected_source = source,
            .selected_partition_numbers = selected_partition_numbers,
            .final_path = destination,
            .created_utc = current_utc_timestamp(),
            .app_version = YTEC_PROJECT_VERSION,
            .encryption_password = password != nullptr
                ? std::optional<std::string_view>(password->view())
                : std::nullopt,
            .verification_mode = verification_mode,
            .replace_existing = replace_existing,
            .rescue_mode = rescue_mode,
            .callbacks = std::move(callbacks),
        };
    auto execution = persistent_exact
        ? ytec::winpeapp::
              execute_direct_image_create_resume_with_windows_apis_v1(
                  request,
                  {
                      .action = resume_existing
                          ? ytec::winpeapp::
                                DirectImageCreateResumeAction::resume_existing
                          : ytec::winpeapp::
                                DirectImageCreateResumeAction::start_new,
                      .new_operation_id = new_operation_id,
                      .reviewed_existing_slot = resume_existing
                          ? resume_observation->binding
                          : std::nullopt,
                  })
        : ytec::winpeapp::execute_direct_image_create_with_windows_apis(
              request);
    payload->success = execution.has_value() &&
        direct_image_create_completion_verified(execution.value());
    if (execution && payload->success) {
      payload->output = format_direct_image_result(execution.value());
    } else if (execution) {
      payload->output =
          L".tsumugi作成の必須証拠が一つでも不完全なため、"
          L"完成扱いにしません。";
    } else {
      payload->output = format_error(
          execution.error(), &payload->error_presentation);
    }
    if (payload->output.empty()) {
      payload->output = payload->success
          ? verification_mode ==
                  ytec::imageformat::TsumugiCreateVerificationMode::complete
              ? L".tsumugiイメージの作成と完全検証が完了しました。"
              : L".tsumugiイメージの作成と高速検証が完了しました。"
          : L".tsumugiイメージを安全に完了できませんでした。";
    }
    if (persistent_exact) {
      auto inspected = inspect_product_image_create_resume();
      if (!inspected) {
        payload->resume_platform_ready = false;
        payload->resume_slot_present = true;
        payload->output +=
            L"\r\n\r\n停止後のactive.checkpointを安全に再確認できません。"
            L"再起動後のstartup確認まで新しい書込み操作を開始しないでください。";
      } else {
        payload->resume_platform_ready = true;
        payload->resume_slot_present =
            inspected.value().object_state != ytec::operationcore::
                PersistentPeExactImageCreateObjectState::no_slot;
        if (payload->resume_slot_present &&
            inspected.value().object_state != ytec::operationcore::
                PersistentPeExactImageCreateObjectState::other_capability) {
          payload->resume_observation_after_operation =
              inspected.take_value();
        }
      }
    }
    pause_controller->mark_completed();
    const auto sleep_prevention_release = sleep_prevention.release();
    payload->completion_power_proof =
        ytec::winpeapp::make_winpe_completion_power_proof(
            ytec::winpeapp::WinPeCompletionPowerOperation::image_create,
            completion_operation_outcome(
                payload->success, rescue_mode && payload->success),
            payload->success,
            sleep_prevention_release,
            completion_power_binding);
    if (payload->success && sleep_prevention_release !=
            ytec::clonecore::SleepPreventionReleaseState::released) {
      payload->output +=
          L"\r\n\r\n自動スリープ防止の解除を確認できないため、"
          L"完了後の再起動／シャットダウンは提示しません。";
    }
    post_payload(window, kImageCreateCompleteMessage, std::move(payload));
  }).detach();
}

void request_image_cancellation(AppState& state) {
  if (!state.image_progress_active ||
      !state.image_progress.cancellation_allowed ||
      state.image_cancellation == nullptr) {
    return;
  }
  state.image_cancellation->store(true, std::memory_order_relaxed);
  if (state.image_pause_controller != nullptr) {
    static_cast<void>(state.image_pause_controller->request_cancel());
  }
  SetWindowTextW(state.image_cancel, L"取消要求済み");
  std::wstring output = control_text(state.image_output);
  output +=
      L"\r\n\r\n取消要求を受け付けました。安全なチャンク境界で停止します。"
      L"\r\n完成名へ確定済みの場合は中断せず、検証結果を返します。";
  if (!state.reviewed_image_rescue_mode) {
    output +=
        L"\r\n通常（exact）作成では、検証済みprefixのpartial/journalと"
        L"active.checkpointを保持し、次回の完全再証明後にsuffixだけを再開します。";
  }
  SetWindowTextW(state.image_output, output.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
}

void choose_restore_image(AppState& state) {
  if (state.operation_busy || state.inventory_busy) {
    return;
  }
  std::vector<wchar_t> path(32768U, L'\0');
  const std::wstring current = control_text(state.restore_path);
  if (current.size() >= path.size()) {
    SetWindowTextW(
        state.restore_output,
        L"復元イメージのパスが長すぎるため、選択画面を開けません。");
    return;
  }
  std::copy(current.begin(), current.end(), path.begin());
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = state.window;
  dialog.lpstrFilter =
      L"Y-TEC Tsumugi (*.tsumugi)\0*.tsumugi\0\0";
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"tsumugi";
  dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
      OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_HIDEREADONLY;
  if (GetOpenFileNameW(&dialog) == FALSE) {
    return;
  }
  SetWindowTextW(state.restore_path, path.data());
  SetFocus(state.restore_verify);
}

void start_restore_image_verification(AppState& state) {
  const std::wstring entered_path = control_text(state.restore_path);
  if (state.operation_busy || state.inventory_busy || entered_path.empty()) {
    return;
  }
  invalidate_restore_image_review(state, L"");

  const auto canonical = canonical_restore_image_path(entered_path);
  if (!canonical) {
    set_error_output_and_show(
        state.window, state.restore_output, canonical.error());
    update_action_state(state);
    return;
  }
  const auto header_probe =
      ytec::imageformat::probe_tsumugi_file_header_v1(canonical.value());
  const auto initial_password_decision =
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt({
          .header_probe_succeeded = header_probe.has_value(),
          .encrypted = header_probe.has_value() &&
              header_probe.value().encrypted,
      });
  if (initial_password_decision ==
      ytec::winpeapp::WinPeTsumugiRestorePasswordPromptDecision::stop) {
    if (header_probe) {
      SetWindowTextW(
          state.restore_output,
          L"固定ヘッダーから暗号化状態を安全に判定できませんでした。"
          L"対象ディスクは変更していません。");
    } else {
      set_error_output_and_show(
          state.window,
          state.restore_output,
          header_probe.error(),
          L"対象ディスクは変更していません。");
    }
    update_action_state(state);
    return;
  }
  if (!confirm_long_operation_power(
          state.window, L"復元イメージの完全検証")) {
    return;
  }

  std::shared_ptr<SecureAsciiPassword> password;
  if (initial_password_decision ==
      ytec::winpeapp::WinPeTsumugiRestorePasswordPromptDecision::
          prompt_required) {
    auto prompt = prompt_tsumugi_password(
        state.window,
        state.body_font,
        L"固定ヘッダーは暗号化済みイメージを示しています。"
        L"完全検証と復元に同じパスワードを使用します。"
        L"回復キーはなく、パスワードは完了または停止時にメモリから消去します。",
        L"暗号化イメージの完全検証",
        false,
        false,
        L"完全検証を開始");
    const auto completed_password_decision =
        ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt({
            .header_probe_succeeded = true,
            .encrypted = true,
            .prompt_completed = true,
            .prompt_accepted = prompt.accepted,
            .password_available = prompt.password != nullptr,
        });
    if (completed_password_decision !=
        ytec::winpeapp::WinPeTsumugiRestorePasswordPromptDecision::
            password_ready) {
      SetWindowTextW(
          state.restore_output,
          L"暗号化パスワードの入力を取り消しました。"
          L"対象ディスクは変更していません。");
      update_action_state(state);
      return;
    }
    password = std::move(prompt.password);
  }

  const bool expected_encrypted = header_probe.value().encrypted;
  const std::wstring requested_path = canonical.value();
  state.operation_busy = true;
  state.restore_progress_active = true;
  state.restore_write_active = false;
  state.restore_step = 1;
  state.restore_elapsed = std::chrono::milliseconds{};
  state.restore_progress = ytec::clonecore::DiskOperationProgress{
      .stage = ytec::clonecore::DiskOperationStage::verifying_source,
      .cancellation_allowed = true,
  };
  state.restore_cancellation = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = state.restore_cancellation;
  SetWindowTextW(
      state.restore_output,
      L"選択した .tsumugi を読取り専用で完全検証しています。\r\n"
      L"ヘッダー、マニフェスト、全チャンク、全体SHA-256を確認します。\r\n"
      L"この段階では復元先ディスクを変更しません。");
  SetWindowTextW(state.restore_cancel, L"安全に取消");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([
      window,
      requested_path,
      expected_encrypted,
      password,
      cancellation]() {
    auto payload = std::make_unique<ImageRestoreVerifyPayload>();
    ThreadSleepPrevention sleep_prevention;
    if (!sleep_prevention.active()) {
      payload->output =
          L"復元イメージ検証中の自動スリープ防止を確立できないため、"
          L"ディスクを変更せずに停止しました。";
      post_payload(
          window, kImageRestoreVerifyCompleteMessage, std::move(payload));
      return;
    }
    const auto storage = query_restore_image_file_system(requested_path);
    if (!storage) {
      payload->output = format_error(
          storage.error(), &payload->error_presentation);
      post_payload(
          window, kImageRestoreVerifyCompleteMessage, std::move(payload));
      return;
    }
    const auto storage_disk =
        ytec::diskmodel::query_single_disk_number_for_local_path(
            requested_path);
    if (!storage_disk) {
      payload->output = format_error(
          storage_disk.error(), &payload->error_presentation) +
          L"\r\nネットワーク、複数ディスク、または物理媒体を一意に"
          L"特定できない保存元は使用できません。";
      post_payload(
          window, kImageRestoreVerifyCompleteMessage, std::move(payload));
      return;
    }
    const auto initial_storage_identity =
        ytec::winpeapp::
            identify_direct_shrink_image_backing_with_windows_apis(
                requested_path);
    const auto started = std::chrono::steady_clock::now();
    auto last_posted = std::make_shared<std::chrono::steady_clock::time_point>(
        started - std::chrono::seconds(1));
    ytec::clonecore::DiskOperationCallbacks callbacks{
        .progress =
            [window, started, last_posted](
                const ytec::clonecore::DiskOperationProgress& progress) {
              const auto now = std::chrono::steady_clock::now();
              if (now - *last_posted < std::chrono::milliseconds(100)) {
                return;
              }
              *last_posted = now;
              auto update = std::make_unique<ImageRestoreProgressPayload>();
              update->progress = progress;
              update->elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - started);
              post_payload(
                  window, kImageRestoreProgressMessage, std::move(update));
            },
        .cancellation_requested =
            [cancellation]() {
              return cancellation->load(std::memory_order_relaxed);
            },
    };
    const ytec::imageformat::TsumugiImageVerifyRequest request{
        .image_path = requested_path,
        .storage_file_system = storage.value(),
        .password = password != nullptr
            ? std::optional<std::string_view>(password->view())
            : std::nullopt,
    };
    auto verified = ytec::imageformat::verify_tsumugi_image_v1(
        request, callbacks);
    const auto verification_gate =
        ytec::winpeapp::evaluate_winpe_tsumugi_complete_verification_gate(
            expected_encrypted,
            password != nullptr,
            verified.has_value());
    if (!verification_gate.target_selection_allowed || !verified) {
      payload->output = !verified
          ? format_error(
                verified.error(), &payload->error_presentation) +
              (expected_encrypted
                   ? L"\r\n\r\nパスワードが一致しない、またはイメージの認証に失敗しました。"
                     L"対象ディスクは変更していません。"
                   : L"\r\n\r\n完全検証に失敗しました。対象ディスクは変更していません。")
          : L"完全検証の認証ゲートを通過できませんでした。"
            L"対象ディスクは変更していません。";
      post_payload(
          window, kImageRestoreVerifyCompleteMessage, std::move(payload));
      return;
    }
    const auto& image = verified.value();
    const auto feature_bits = image.container.header.required_features;
    const auto encrypted_bit = static_cast<std::uint32_t>(
        ytec::imageformat::TsumugiRequiredFeature::encrypted);
    const bool actual_encrypted = (feature_bits & encrypted_bit) != 0U;
    if (actual_encrypted != expected_encrypted) {
      payload->output =
          L"固定ヘッダー確認後にイメージの暗号化状態が変化しました。"
          L"ファイルを選び直してください。対象ディスクは変更していません。";
    } else if (
        image.manifest.mode ==
            ytec::imageformat::TsumugiManifestMode::shrink &&
        (image.partial_loss || image.manifest.logical_sector_size != 512U)) {
      payload->output =
          L"縮小移行の製品復元は、欠損のない512-byte logical sectorの"
          L"イメージだけに対応します。対象ディスクは変更していません。";
    } else if (
        image.manifest.mode !=
            ytec::imageformat::TsumugiManifestMode::exact &&
        image.manifest.mode !=
            ytec::imageformat::TsumugiManifestMode::rescue) {
      payload->output =
          L"未知の復元モードです。対象ディスクは変更していません。";
    } else {
      std::optional<ytec::clonecore::StableDiskIdentity>
          stable_storage_identity;
      if (image.manifest.mode ==
          ytec::imageformat::TsumugiManifestMode::shrink) {
        if (!initial_storage_identity) {
          payload->output = format_error(
              initial_storage_identity.error(),
              &payload->error_presentation) +
              L"\r\n縮小移行ではイメージ保存元を安定識別できないため、"
              L"対象ディスクを変更せずに停止しました。";
          post_payload(
              window,
              kImageRestoreVerifyCompleteMessage,
              std::move(payload));
          return;
        }
        auto final_storage_identity =
            ytec::winpeapp::
                identify_direct_shrink_image_backing_with_windows_apis(
                    requested_path);
        if (!final_storage_identity) {
          payload->output = format_error(
              final_storage_identity.error(),
              &payload->error_presentation) +
              L"\r\n完全検証後にイメージ保存元を再識別できませんでした。"
              L"対象ディスクは変更していません。";
          post_payload(
              window,
              kImageRestoreVerifyCompleteMessage,
              std::move(payload));
          return;
        }
        const auto stable = ytec::clonecore::validate_stable_identity(
            initial_storage_identity.value(),
            final_storage_identity.value(),
            L"WinPE縮小復元イメージ保存元");
        if (!stable ||
            final_storage_identity.value().disk_number !=
                storage_disk.value()) {
          payload->output =
              L"完全検証の前後でイメージ保存元の物理ディスクが変化しました。"
              L"対象ディスクは変更していません。再選択してください。";
          post_payload(
              window,
              kImageRestoreVerifyCompleteMessage,
              std::move(payload));
          return;
        }
        stable_storage_identity = final_storage_identity.take_value();
      }
      payload->selection = VerifiedRestoreImageSelection{
          .path = requested_path,
          .storage_file_system = storage.value(),
          .storage_disk_number = storage_disk.value(),
          .stable_storage_identity =
              std::move(stable_storage_identity),
          .global_hash = image.container.global_hash,
          .source_state_hash = image.manifest.source_state_hash,
          .manifest = image.manifest,
          .mode = image.manifest.mode,
          .partition_style = image.manifest.partition_style,
          .source_disk_size = image.manifest.source_disk_size,
          .logical_sector_size = image.manifest.logical_sector_size,
          .partition_count = image.manifest.partitions.size(),
          .partial_loss = image.partial_loss,
          .encrypted = actual_encrypted,
      };
      if (actual_encrypted) {
        payload->password = password;
      }
      payload->output = format_verified_restore_image(
          payload->selection.value());
    }
    post_payload(
        window, kImageRestoreVerifyCompleteMessage, std::move(payload));
  }).detach();
}

void start_restore_target_review(AppState& state) {
  const auto selected = selected_disk_number(state.restore_target);
  if (state.operation_busy || state.inventory_busy ||
      !state.verified_restore_image.has_value() || !selected.has_value()) {
    return;
  }
  invalidate_restore_target_review(state, L"");
  const auto target = find_inventory_disk(state, selected.value());
  if (!target.has_value()) {
    SetWindowTextW(
        state.restore_output,
        L"復元先を現在の読取り専用一覧で再確認できません。再読込みしてください。");
    update_action_state(state);
    return;
  }
  const auto image = state.verified_restore_image.value();
  const bool shrink_restore =
      image.mode == ytec::imageformat::TsumugiManifestMode::shrink;
  const bool individual_selected =
      selected_restore_source_partition(state).has_value();
  const auto individual =
      current_restore_individual_partition_selection(state);
  if (shrink_restore &&
      (individual_selected || individual.has_value() || image.partial_loss ||
       image.logical_sector_size != 512U ||
       !image.stable_storage_identity.has_value())) {
    SetWindowTextW(
        state.restore_output,
        L"縮小移行は、安定識別済み・欠損なし・512-byte logical sectorの"
        L"イメージをディスク全体へ戻す場合だけ実行できます。"
        L"対象ディスクは変更していません。");
    update_action_state(state);
    return;
  }
  if (individual_selected && !individual.has_value()) {
    SetWindowTextW(
        state.restore_output,
        L"必要容量を満たす既存パーティションまたは未割当候補を選択してください。");
    update_action_state(state);
    return;
  }
  if (ytec::winpeapp::is_image_stored_on_restore_target(
          image.storage_disk_number, target->disk_number)) {
    SetWindowTextW(
        state.restore_output,
        L"選択した復元先ディスク上に復元イメージがあります。\r\n"
        L"イメージを保持する別の物理ディスクへ移してから再選択してください。"
        L"このディスクは変更していません。");
    update_action_state(state);
    return;
  }
  if (!shrink_restore && !individual.has_value() &&
      target->size_bytes < image.source_disk_size) {
    SetWindowTextW(
        state.restore_output,
        L"復元先が元ディスクより小さいため通常／救出の全体復元はできません。\r\n"
        L"縮小移行イメージを完全検証して選択してください。対象ディスクは変更していません。");
    update_action_state(state);
    return;
  }
  if ((shrink_restore && target->logical_sector_size != 512U) ||
      (!shrink_restore &&
       target->logical_sector_size != image.logical_sector_size)) {
    SetWindowTextW(
        state.restore_output,
        L"元イメージと復元先の論理セクターサイズが一致しません。\r\n"
        L"ファイル単位移行UIが未接続のため、この経路では復元しません。");
    update_action_state(state);
    return;
  }
  if (individual.has_value()) {
    const auto placement = ytec::imageformat::
        validate_tsumugi_physical_individual_partition_selection_v1(
            image.manifest, target.value(), individual.value());
    if (!placement) {
      set_error_output_and_show(
          state.window, state.restore_output, placement.error());
      update_action_state(state);
      return;
    }
  }
  const auto target_class =
      ytec::imageformat::classify_tsumugi_physical_restore_target(
          target.value());
  const auto safe =
      ytec::imageformat::validate_tsumugi_physical_restore_target(
          target.value(), target_class, false);
  if (!safe) {
    set_error_output_and_show(
        state.window, state.restore_output, safe.error());
    update_action_state(state);
    return;
  }
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      target.value(), target->is_system_disk);
  if (!identity) {
    set_error_output_and_show(
        state.window, state.restore_output, identity.error());
    update_action_state(state);
    return;
  }
  auto layout_hash = ytec::winpeapp::hash_direct_image_restore_target_layout(
      target.value());
  if (!layout_hash) {
    set_error_output_and_show(
        state.window, state.restore_output, layout_hash.error());
    update_action_state(state);
    return;
  }
  if (shrink_restore) {
    auto model_hash = ytec::imageformat::hash_tsumugi_source_model_v1(
        target->model);
    auto serial_hash = ytec::imageformat::hash_tsumugi_source_serial_v1(
        target->serial_suffix, target->device_instance_id);
    if (!model_hash || !serial_hash) {
      set_error_output_and_show(
          state.window,
          state.restore_output,
          !model_hash ? model_hash.error() : serial_hash.error());
      update_action_state(state);
      return;
    }
    std::optional<ytec::clonecore::StableDiskIdentity>
        original_source_target;
    if (model_hash.value() == image.manifest.source_model_hash &&
        serial_hash.value() == image.manifest.source_serial_hash) {
      original_source_target = identity.value();
    }
    auto shrink_layout = ytec::winpeapp::
        make_direct_shrink_image_restore_layout_with_windows_apis(
            image.manifest,
            target.value(),
            identity.value(),
            original_source_target);
    if (!shrink_layout) {
      set_error_output_and_show(
          state.window,
          state.restore_output,
          shrink_layout.error(),
          L"対応外のpayload・形式・容量・セクターでは書込みを開始しません。");
      update_action_state(state);
      return;
    }
    auto work = ytec::winpeapp::
        review_direct_shrink_active_rescue_work_with_windows_apis(
            image.stable_storage_identity.value(), identity.value());
    if (!work) {
      set_error_output_and_show(
          state.window,
          state.restore_output,
          work.error(),
          L"イメージ・復元先・起動媒体dataを三つの異なる物理ディスクへ分けてください。");
      update_action_state(state);
      return;
    }
    state.reviewed_shrink_restore_work = work.take_value();
    state.reviewed_shrink_restore_layout = shrink_layout.take_value();
    state.reviewed_shrink_original_source_target = original_source_target;
  }
  state.reviewed_restore_target = target;
  state.reviewed_restore_target_identity = identity.take_value();
  state.reviewed_restore_target_layout_hash = layout_hash.take_value();
  state.reviewed_restore_individual_partition = individual;
  state.restore_execution_ready = true;
  state.restore_step = 3;
  SetWindowTextW(state.restore_token, L"");
  SetWindowTextW(
      state.restore_output,
      (shrink_restore
           ? format_shrink_restore_target_review(
                 image,
                 target.value(),
                 state.reviewed_shrink_restore_work.value(),
                 state.reviewed_shrink_restore_layout.value(),
                 state.reviewed_shrink_original_source_target.has_value())
           : format_restore_target_review(
                 image, target.value(), individual))
          .c_str());
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  SetFocus(state.restore_token);
}

void start_restore_execute(AppState& state) {
  if (state.operation_busy || !state.restore_execution_ready ||
      !state.verified_restore_image.has_value() ||
      !state.reviewed_restore_target.has_value() ||
      !state.reviewed_restore_target_identity.has_value() ||
      !state.reviewed_restore_target_layout_hash.has_value() ||
      state.reviewed_restore_individual_partition !=
          current_restore_individual_partition_selection(state) ||
      control_text(state.restore_token) != L"OK") {
    return;
  }
  if (!current_process_is_elevated()) {
    SetWindowTextW(
        state.restore_output,
        L"管理者権限を確認できないため復元を開始しません。"
        L"このWinPEを正規レスキューメディアから起動してください。");
    return;
  }
  const auto image = state.verified_restore_image.value();
  const bool shrink_restore =
      image.mode == ytec::imageformat::TsumugiManifestMode::shrink;
  if (shrink_restore &&
      (!image.stable_storage_identity.has_value() ||
       !state.reviewed_shrink_restore_work.has_value() ||
       !state.reviewed_shrink_restore_layout.has_value())) {
    SetWindowTextW(
        state.restore_output,
        L"縮小復元の三媒体識別または最終配置レビューが失効しています。"
        L"上書き内容をもう一度確認してください。");
    invalidate_restore_target_review(
        state, control_text(state.restore_output));
    update_action_state(state);
    return;
  }
  if (image.encrypted && state.reviewed_restore_image_password == nullptr) {
    SetWindowTextW(
        state.restore_output,
        L"暗号化イメージの検証済みパスワードをメモリ上で確認できません。"
        L"イメージを選び直して完全検証してください。対象ディスクは変更していません。");
    invalidate_restore_image_review(state, control_text(state.restore_output));
    update_action_state(state);
    return;
  }
  const auto target = state.reviewed_restore_target.value();
  const bool persistent_resume =
      !shrink_restore &&
      !state.reviewed_restore_individual_partition.has_value();
  if (persistent_resume && !state.restore_resume_platform_ready) {
    SetWindowTextW(
        state.restore_output,
        L"実行ファイル隣の data と起動媒体を永続再開用として安全に確認できないため、"
        L"ディスク全体復元は開始しません。対象ディスクは変更していません。");
    return;
  }
  if (state.restore_resume_slot_present &&
      !state.restore_resume_requested) {
    SetWindowTextW(
        state.restore_output,
        L"前回の再開情報を保持中です。新しいディスク全体復元は開始しません。"
        L"アプリを再起動し、再開または破棄を選択してください。");
    return;
  }
  if (state.restore_resume_requested &&
      (!persistent_resume || !state.reviewed_restore_resume_slot)) {
    SetWindowTextW(
        state.restore_output,
        L"前回のディスク全体復元を再開するには、同じイメージとディスク全体を選択してください。"
        L"個別パーティション復元へ切り替えません。");
    return;
  }
  const auto current_image_disk =
      ytec::diskmodel::query_single_disk_number_for_local_path(image.path);
  if (!current_image_disk) {
    set_error_output_and_show(
        state.window,
        state.restore_output,
        current_image_disk.error(),
        L"イメージ保存媒体を再確認してください。復元先は変更していません。");
    invalidate_restore_image_review(state, control_text(state.restore_output));
    update_action_state(state);
    return;
  }
  if (current_image_disk.value() != image.storage_disk_number ||
      current_image_disk.value() == target.disk_number) {
    SetWindowTextW(
        state.restore_output,
        L"完全検証後にイメージ保存媒体が変化したか、"
        L"復元先と同じ物理ディスクです。再選択してください。");
    invalidate_restore_image_review(state, control_text(state.restore_output));
    update_action_state(state);
    return;
  }
  if (shrink_restore) {
    auto current_image_identity = ytec::winpeapp::
        identify_direct_shrink_image_backing_with_windows_apis(image.path);
    const auto stable = current_image_identity
        ? ytec::clonecore::validate_stable_identity(
              image.stable_storage_identity.value(),
              current_image_identity.value(),
              L"WinPE縮小復元イメージ保存元実行前確認")
        : ytec::clonecore::Status::failure(current_image_identity.error());
    if (!stable) {
      set_error_output_and_show(
          state.window,
          state.restore_output,
          stable.error(),
          L"完全検証後にイメージ保存媒体が変化しました。復元先は変更していません。");
      invalidate_restore_image_review(
          state, control_text(state.restore_output));
      update_action_state(state);
      return;
    }
  }
  if (!confirm_long_operation_power(state.window, L"イメージ復元")) {
    return;
  }

  const auto target_identity = state.reviewed_restore_target_identity.value();
  const auto target_layout_hash =
      state.reviewed_restore_target_layout_hash.value();
  const auto individual_partition =
      state.reviewed_restore_individual_partition;
  const auto resume_binding = state.reviewed_restore_resume_slot;
  const bool resume_existing = state.restore_resume_requested;
  const auto shrink_work = state.reviewed_shrink_restore_work;
  const auto shrink_layout = state.reviewed_shrink_restore_layout;
  const auto shrink_original_source_target =
      state.reviewed_shrink_original_source_target;
  ytec::operationcore::OperationId new_operation_id{};
  ytec::operationcore::OperationId admission_operation_id{};
  if (!resume_existing) {
    auto generated = make_operation_id(
        persistent_resume
            ? L"PE永続復元操作ID"
            : L"PE直接復元resume-slot admission ID");
    if (!generated) {
      set_error_output_and_show(
          state.window, state.restore_output, generated.error());
      return;
    }
    admission_operation_id = generated.take_value();
    if (persistent_resume) {
      new_operation_id = admission_operation_id;
    }
  }
  if (resume_existing) {
    if (!resume_binding.has_value() ||
        !guard_bound_product_restore_resume(
            state.window, state.restore_output, resume_binding.value())) {
      return;
    }
  } else {
    std::wstring individual_binding = L"whole-disk";
    if (individual_partition.has_value()) {
      individual_binding = L"source-table-index=" + std::to_wstring(
          individual_partition->source_table_index);
      if (const auto* existing = std::get_if<ytec::imageformat::
              TsumugiPhysicalExistingPartitionRestoreSelection>(
              &individual_partition->target)) {
        individual_binding +=
            L";existing-target=" +
            std::to_wstring(existing->target_table_index) + L":" +
            std::to_wstring(existing->target_partition_number) + L":" +
            std::to_wstring(existing->target_offset) + L":" +
            std::to_wstring(existing->target_size);
      } else if (const auto* unallocated = std::get_if<ytec::imageformat::
                     TsumugiPhysicalUnallocatedRestoreSelection>(
                     &individual_partition->target)) {
        individual_binding +=
            L";unallocated-target=" +
            std::to_wstring(unallocated->target_offset) + L":" +
            std::to_wstring(unallocated->target_size);
      } else {
        SetWindowTextW(
            state.restore_output,
            L"個別復元の最終配置をSingleResumeSlot gateへ拘束できないため、"
            L"復元先を変更せずに停止しました。");
        return;
      }
    }
    const std::wstring mode_binding =
        image.mode == ytec::imageformat::TsumugiManifestMode::shrink
        ? L"shrink"
        : image.mode == ytec::imageformat::TsumugiManifestMode::rescue
        ? L"rescue"
        : L"exact";
    const std::array<std::wstring, 6U> admission_owned_fields{
        image.path,
        mode_binding,
        individual_binding,
        persistent_resume ? L"persistent-start-new" : L"direct-restore",
        image.encrypted ? L"encrypted" : L"unencrypted",
        shrink_restore ? L"shrink-layout-reviewed" : L"exact-layout-reviewed",
    };
    const std::array<std::wstring_view, 6U> admission_fields{
        admission_owned_fields[0],
        admission_owned_fields[1],
        admission_owned_fields[2],
        admission_owned_fields[3],
        admission_owned_fields[4],
        admission_owned_fields[5],
    };
    const std::array<ytec::operationcore::Sha256Digest, 3U>
        admission_digests{
            image.global_hash,
            image.source_state_hash,
            target_layout_hash,
        };
    auto resume_slot_admission = ytec::winpeapp::
        make_winpe_resume_slot_admission_plan(
            admission_operation_id,
            ytec::operationcore::OperationKind::image_restore,
            std::nullopt,
            target_identity,
            image.source_disk_size,
            admission_fields,
            admission_digests);
    if (!guard_new_product_operation_start(
            state.window, state.restore_output, resume_slot_admission)) {
      return;
    }
  }
  const std::uint64_t completion_power_binding =
      ytec::winpeapp::take_winpe_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  const std::wstring completion_power_target =
      format_completion_disk_target(target_identity);
  const auto password = state.reviewed_restore_image_password;
  state.reviewed_restore_image_password.reset();
  state.operation_busy = true;
  state.restore_execution_ready = false;
  state.restore_progress_active = true;
  state.restore_write_active = true;
  state.restore_step = 4;
  state.restore_elapsed = std::chrono::milliseconds{};
  state.restore_progress = ytec::clonecore::DiskOperationProgress{
      .stage = ytec::clonecore::DiskOperationStage::verifying_source,
      .cancellation_allowed = true,
  };
  state.restore_cancellation = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = state.restore_cancellation;
  state.restore_pause_controller =
      make_ui_manual_pause_controller(state.window);
  const auto pause_controller = state.restore_pause_controller;
  SetWindowTextW(
      state.restore_output,
      L"実行前の完全再検証と、起動中レスキュー媒体の安定識別を行っています。\r\n"
      L"検証合格後だけ復元先をオフラインにし、全書込みを読戻します。\r\n"
      L"取消は安全な境界で処理し、対象はオフラインのまま保持します。");
  SetWindowTextW(state.restore_cancel, L"安全に取消");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  std::thread([
      window,
      image,
      target,
      shrink_restore,
      shrink_work,
      shrink_layout,
      shrink_original_source_target,
      target_identity,
       target_layout_hash,
       individual_partition,
       persistent_resume,
       resume_existing,
       resume_binding,
       new_operation_id,
       password,
       completion_power_binding,
       completion_power_target,
       cancellation,
       pause_controller]() {
    auto payload = std::make_unique<ImageRestorePayload>();
    payload->completion_power_target = completion_power_target;
    bool mandatory_completion_verified = false;
    ThreadSleepPrevention sleep_prevention;
    if (!sleep_prevention.active()) {
      payload->success = false;
      payload->output =
          L"復元中の自動スリープ防止を確立できないため、"
          L"復元先ディスクを変更せずに停止しました。";
      pause_controller->mark_completed();
      post_payload(
          window, kImageRestoreCompleteMessage, std::move(payload));
      return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto last_posted = std::make_shared<std::chrono::steady_clock::time_point>(
        started - std::chrono::seconds(1));
    ytec::clonecore::DiskOperationCallbacks callbacks{
        .progress =
            [window, started, last_posted](
                const ytec::clonecore::DiskOperationProgress& progress) {
              const auto now = std::chrono::steady_clock::now();
              if (now - *last_posted < std::chrono::milliseconds(100)) {
                return;
              }
              *last_posted = now;
              auto update = std::make_unique<ImageRestoreProgressPayload>();
              update->progress = progress;
              update->elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - started);
              post_payload(
                  window, kImageRestoreProgressMessage, std::move(update));
            },
        .cancellation_requested =
            [cancellation]() {
              return cancellation->load(std::memory_order_relaxed);
        },
    };
    callbacks = ytec::clonecore::bind_manual_pause_controller(
        std::move(callbacks), pause_controller);
    if (shrink_restore) {
      payload->shrink_restore = true;
      ytec::winpeapp::DirectShrinkImageRestoreRequest request{
          .image = {
              .image_path = image.path,
              .storage_file_system = image.storage_file_system,
              .password = password != nullptr
                  ? std::optional<std::string_view>(password->view())
                  : std::nullopt,
          },
          .expected_image_global_hash = image.global_hash,
          .expected_source_state_hash = image.source_state_hash,
          .reviewed_manifest = image.manifest,
          .reviewed_image_partial_loss = image.partial_loss,
          .image_backing_disk = image.stable_storage_identity.value(),
          .reviewed_target = target,
          .expected_target = target_identity,
          .reviewed_original_source_target =
              shrink_original_source_target,
          .expected_target_layout_hash = target_layout_hash,
          .reviewed_layout = shrink_layout.value(),
          .work = shrink_work.value(),
          .confirmation = {
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
          .administrator = true,
          .callbacks = std::move(callbacks),
      };
      auto execution = ytec::winpeapp::
          execute_direct_shrink_image_restore_with_windows_apis(request);
      payload->success = execution.has_value();
      payload->partial_loss = image.partial_loss;
      payload->boot_repair_offer_required = execution.has_value() &&
          execution.value().boot_repair_offer_required;
      mandatory_completion_verified = execution.has_value() &&
          !payload->partial_loss &&
          direct_shrink_restore_completion_verified(execution.value());
      if (execution) {
        payload->output = format_direct_shrink_restore_result(
            execution.value());
      } else {
        payload->output = format_error(
            execution.error(), &payload->error_presentation);
      }
    } else {
      ytec::winpeapp::DirectImageRestoreRequest request{
          .image = {
              .image_path = image.path,
              .storage_file_system = image.storage_file_system,
              .password = password != nullptr
                  ? std::optional<std::string_view>(password->view())
                  : std::nullopt,
          },
          .expected_image_global_hash = image.global_hash,
          .expected_source_state_hash = image.source_state_hash,
          .expected_target = target_identity,
          .expected_target_layout_hash = target_layout_hash,
          .individual_partition = individual_partition,
          .confirmation = {
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
          .administrator = true,
          .callbacks = std::move(callbacks),
      };
      if (!persistent_resume) {
        auto execution =
            ytec::winpeapp::execute_direct_image_restore_with_windows_apis(
                request,
                ytec::winpeapp::
                    query_active_rescue_media_target_with_windows_apis);
        payload->success = execution.has_value();
        payload->partial_loss = execution.has_value() &&
            execution.value().physical.partial_loss;
        payload->boot_repair_offer_required = execution.has_value() &&
            execution.value().physical.boot_repair_offer_required;
        mandatory_completion_verified = execution.has_value() &&
            !payload->partial_loss &&
            direct_image_restore_completion_verified(execution.value());
        if (execution) {
          payload->output = format_direct_restore_result(
              execution.value(), true);
        } else {
          payload->output = format_error(
              execution.error(), &payload->error_presentation);
        }
      } else {
      payload->persistent_resume_attempt = true;
      auto storage = ytec::winpeapp::
          make_direct_image_restore_windows_storage_platform_v1();
      if (!storage) {
        payload->output = format_error(
            storage.error(), &payload->error_presentation);
      } else {
        auto slot = ytec::operationcore::
            make_current_executable_windows_resume_slot_platform(
                storage.value().prove_data_backing);
        if (!slot) {
          payload->output = format_error(
              slot.error(), &payload->error_presentation);
        } else {
          auto backend = ytec::winpeapp::
              make_direct_image_restore_windows_resume_backend_v1(
                  *slot.value(),
                  ytec::winpeapp::
                      query_active_rescue_media_target_with_windows_apis,
                  storage.value().prove_restore_storage);
          if (!backend) {
            payload->output = format_error(
                backend.error(), &payload->error_presentation);
          } else {
            auto execution = ytec::winpeapp::
                control_direct_image_restore_resume_v1(
                    request,
                    {
                        .action = resume_existing
                            ? ytec::winpeapp::
                                  DirectImageRestoreResumeAction::
                                      resume_existing
                            : ytec::winpeapp::
                                  DirectImageRestoreResumeAction::start_new,
                        .new_operation_id = new_operation_id,
                        .reviewed_existing_slot = resume_binding,
                    },
                    backend.value()->dependencies());
            const bool completed = execution &&
                (execution.value().kind == ytec::winpeapp::
                     DirectImageRestoreResumeOutcomeKind::completed ||
                 execution.value().kind == ytec::winpeapp::
                     DirectImageRestoreResumeOutcomeKind::
                         completed_checkpoint_retained) &&
                execution.value().transfer.has_value();
            payload->success = completed;
            if (completed) {
              payload->partial_loss = execution.value().partial_loss;
              mandatory_completion_verified =
                  persistent_restore_completion_verified(execution.value());
              payload->output = format_persistent_restore_result(
                  execution.value());
            } else if (!execution) {
              payload->output = format_error(
                  execution.error(), &payload->error_presentation);
            } else {
              payload->output =
                  L"永続再開controllerが完了状態を返さなかったため、"
                  L"完了扱いにしません。checkpointは保持します。";
            }
          }

          const ytec::winpeapp::DirectImageRestoreResumeDependencies
              inspect_deps{.slot_platform = slot.value().get()};
          auto after = ytec::winpeapp::
              control_direct_image_restore_resume_v1(
                  ytec::winpeapp::DirectImageRestoreRequest{},
                  {
                      .action = ytec::winpeapp::
                          DirectImageRestoreResumeAction::inspect_only,
                  },
                  inspect_deps);
          payload->resume_platform_ready = after.has_value();
          if (after && after.value().kind == ytec::winpeapp::
                  DirectImageRestoreResumeOutcomeKind::decision_required &&
              after.value().existing_slot) {
            payload->resume_slot_present = true;
            payload->reviewed_resume_slot_after_operation =
                after.value().existing_slot;
          }
        }
      }
      }
    }
    pause_controller->mark_completed();
    const auto sleep_prevention_release = sleep_prevention.release();
    payload->completion_power_proof =
        ytec::winpeapp::make_winpe_completion_power_proof(
            ytec::winpeapp::WinPeCompletionPowerOperation::image_restore,
            completion_operation_outcome(
                payload->success,
                payload->success && payload->partial_loss),
            mandatory_completion_verified,
            sleep_prevention_release,
            completion_power_binding);
    if (payload->success && sleep_prevention_release !=
            ytec::clonecore::SleepPreventionReleaseState::released) {
      payload->output +=
          L"\r\n\r\n自動スリープ防止の解除を確認できないため、"
          L"完了後の再起動／シャットダウンは提示しません。";
    }
    post_payload(window, kImageRestoreCompleteMessage, std::move(payload));
  }).detach();
}

void request_restore_cancellation(AppState& state) {
  if (!state.restore_progress_active ||
      !state.restore_progress.cancellation_allowed ||
      state.restore_cancellation == nullptr) {
    return;
  }
  state.restore_cancellation->store(true, std::memory_order_relaxed);
  if (state.restore_pause_controller != nullptr) {
    static_cast<void>(state.restore_pause_controller->request_cancel());
  }
  SetWindowTextW(state.restore_cancel, L"取消要求済み");
  std::wstring output = control_text(state.restore_output);
  output +=
      L"\r\n\r\n取消要求を受け付けました。安全なチャンク境界で停止します。"
      L"\r\n復元先を変更した場合は、オフラインの未完了状態を維持します。";
  SetWindowTextW(state.restore_output, output.c_str());
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
}

void start_winre_diagnostic(AppState& state) {
  const auto disk_number = selected_disk_number(state.repair_disk);
  const std::wstring windows_root = control_text(state.windows_root);
  if (state.operation_busy || !disk_number.has_value() ||
      !is_drive_root(windows_root)) {
    return;
  }

  invalidate_boot_review(state);
  state.operation_busy = true;
  SetWindowTextW(
      state.repair_output,
      L"Microsoft署名済みREAgentCの /info だけを使い、WinREの登録先と"
      L"Winre.wimを読み取り確認しています。\r\n"
      L"BCD、回復設定、ディスクは変更しません。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);

  const HWND window = state.window;
  const std::wstring offline_windows_directory =
      windows_root + L"Windows";
  std::thread(
      [window,
       offline_windows_directory,
       disk_number = disk_number.value()]() {
        auto payload = std::make_unique<WinReDiagnosticPayload>();
        auto service =
            ytec::bootrepair::make_windows_winre_diagnostic_service();
        const auto result =
            service->inspect(offline_windows_directory, disk_number);
        if (result) {
          const auto view =
              ytec::winpeapp::build_winre_diagnostic_view(result.value());
          payload->output = view.headline + L"\r\n\r\n" + view.details;
        } else {
          payload->output = format_error(
              result.error(), &payload->error_presentation) +
              L"\r\n\r\n診断からMBR→GPT再構築の可否を推測せず、停止します。";
        }
        post_payload(
            window,
            kWinReDiagnosticCompleteMessage,
            std::move(payload));
      })
      .detach();
}

void start_boot_inspect(AppState& state) {
  const auto disk_number = selected_disk_number(state.repair_disk);
  if (state.operation_busy || state.inventory_busy ||
      !disk_number.has_value()) {
    return;
  }
  const auto selected_disk = find_inventory_disk(state, disk_number.value());
  if (!selected_disk.has_value()) {
    SetWindowTextW(
        state.repair_output,
        L"選択したディスクを現在の一覧で確認できません。再読込みしてください。");
    return;
  }
  state.operation_busy = true;
  state.boot_execution_active = false;
  state.repair_step = 2;
  state.inspected_boot_plan.reset();
  state.inspected_system_partition_creation.reset();
  state.inspected_boot_choices.reset();
  state.inspected_boot_execution.reset();
  state.inspected_efi_delete_plan.reset();
  state.inspected_boot_selections.clear();
  state.boot_confirmation_token.clear();
  SetWindowTextW(state.repair_token, L"");
  SetWindowTextW(
      state.repair_output,
      L"対象ディスクを安定再識別し、Windows、WinRE、ESP／Active、"
      L"MBR／GPT、起動方式を\r\n読み取り専用で自動解析しています。"
      L"ドライブ文字、BCD、ディスクは変更しません。");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([window, selected_disk = selected_disk.value()]() {
    auto payload = std::make_unique<BootInspectPayload>();
    auto planner =
        ytec::bootrepair::make_windows_automatic_boot_repair_plan_service();
    if (planner == nullptr) {
      payload->error = L"自動起動修復の読取り専用解析を初期化できません。";
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    auto planned = planner->plan(selected_disk);
    if (!planned) {
      payload->error = format_error(
          planned.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    payload->output = format_automatic_boot_repair_plan(
        planned.value(), nullptr);
    payload->plan = planned.take_value();
    post_payload(window, kBootInspectCompleteMessage, std::move(payload));
  }).detach();
}

void start_system_partition_creation_review(
    AppState& state,
    ytec::bootrepair::AutomaticBootRepairPlan plan,
    const std::uint32_t windows_partition_number) {
  state.operation_busy = true;
  state.boot_execution_active = false;
  state.repair_step = 2;
  state.inspected_boot_plan.reset();
  state.inspected_system_partition_creation.reset();
  state.inspected_boot_choices.reset();
  state.inspected_boot_execution.reset();
  state.inspected_efi_delete_plan.reset();
  state.inspected_boot_selections.clear();
  state.boot_confirmation_token.clear();
  SetWindowTextW(state.repair_token, L"");
  SetWindowTextW(
      state.repair_output,
      L"選択したWindows volumeのVDS状態と最大縮小可能量を読み取り専用で確認し、"
      L"新しいESP／Active領域のexact配置をpure reviewしています。\r\n"
      L"この段階ではNTFS、パーティション表、BCD、NVRAMを変更しません。");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([
      window,
      planned = std::move(plan),
      windows_partition_number]() mutable {
    auto payload = std::make_unique<BootInspectPayload>();
    payload->output = format_automatic_boot_repair_plan(planned, nullptr);
    auto platform = ytec::bootrepair::
        make_windows_system_partition_creation_platform();
    if (platform == nullptr) {
      payload->error =
          L"システム領域作成のMicrosoft VDS安全境界を初期化できません。";
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    auto observation = platform->observe_read_only(
        planned.selected_identity, windows_partition_number);
    if (!observation) {
      payload->error = format_error(
          observation.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    auto reviewed = ytec::bootrepair::review_system_partition_creation(
        planned, windows_partition_number, observation.value());
    if (!reviewed) {
      payload->error = format_error(
          reviewed.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    payload->output +=
        format_system_partition_creation_review(reviewed.value());
    payload->system_partition_creation = reviewed.take_value();
    post_payload(window, kBootInspectCompleteMessage, std::move(payload));
  }).detach();
}

void start_boot_review(
    AppState& state,
    ytec::bootrepair::AutomaticBootRepairPlan plan,
    ytec::winpeapp::WinPeAutomaticBootRepairProductChoice product_choice) {
  state.operation_busy = true;
  state.boot_execution_active = false;
  state.repair_step = 2;
  state.inspected_system_partition_creation.reset();
  SetWindowTextW(
      state.repair_output,
      L"明示選択したWindows登録順をpure reviewへ束縛し、"
      L"各Windowsと既存システム領域を読み取り再確認しています。\r\n"
      L"この段階ではBCD、EFI、WinRE、NVRAMを変更しません。");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([
      window,
      planned = std::move(plan),
      choice = std::move(product_choice)]() mutable {
    auto payload = std::make_unique<BootInspectPayload>();
    payload->output = format_automatic_boot_repair_plan(planned, nullptr);
    auto choice_request = ytec::winpeapp::
        build_product_automatic_boot_repair_choice_request(
            planned, choice);
    if (!choice_request) {
      payload->error = format_error(
          choice_request.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    auto choices =
        ytec::bootrepair::review_automatic_boot_repair_choices(
            planned, choice_request.value());
    if (!choices) {
      payload->error = format_error(
          choices.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    auto execution = ytec::winpeapp::
        build_executable_reviewed_automatic_boot_repair(
            choices.value());
    if (!execution) {
      payload->error = format_error(
          execution.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    auto bound_execution = observe_and_bind_boot_repair_winre_images(
        execution.take_value());
    if (!bound_execution) {
      payload->error = format_error(
          bound_execution.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }

    auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
    auto service = provider == nullptr
        ? nullptr
        : ytec::bootrepair::make_windows_standalone_boot_repair_service(
              *provider);
    if (service == nullptr) {
      payload->error = L"起動修復の安全実行境界を初期化できません。";
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    std::vector<ytec::bootrepair::BootRepairTargetSelection> inspected;
    inspected.reserve(
        bound_execution.value().requests_in_boot_priority.size());
    for (const auto& request :
         bound_execution.value().requests_in_boot_priority) {
      auto one = service->inspect(request);
      if (!one) {
        payload->error = format_error(
            one.error(), &payload->error_presentation);
        post_payload(
            window, kBootInspectCompleteMessage, std::move(payload));
        return;
      }
      inspected.push_back(one.take_value());
    }
    const auto inspection_status =
        ytec::winpeapp::
            validate_reviewed_automatic_boot_repair_inspections(
                planned,
                choices.value(),
                bound_execution.value(),
                inspected);
    if (!inspection_status) {
      payload->error = format_error(
          inspection_status.error(), &payload->error_presentation);
      post_payload(window, kBootInspectCompleteMessage, std::move(payload));
      return;
    }
    std::optional<ytec::bootrepair::ReviewedEfiDeletePlan>
        efi_delete_plan;
    if (bound_execution.value().third_party_efi_delete_requested) {
      auto esp_request = ytec::winpeapp::
          build_windows_efi_delete_esp_request(choices.value());
      if (!esp_request) {
        payload->error = format_error(
            esp_request.error(), &payload->error_presentation);
        post_payload(window, kBootInspectCompleteMessage, std::move(payload));
        return;
      }
      auto esp_identity = ytec::bootrepair::
          inspect_windows_efi_delete_esp_identity_read_only(
              esp_request.value());
      auto inspector = ytec::bootrepair::
          make_windows_efi_delete_read_only_inspector();
      if (!esp_identity || inspector == nullptr) {
        payload->error = !esp_identity
            ? format_error(
                  esp_identity.error(), &payload->error_presentation)
            : L"第三者EFI削除の読取り専用inspectorを初期化できません。";
        post_payload(window, kBootInspectCompleteMessage, std::move(payload));
        return;
      }
      auto reviewed_delete = ytec::bootrepair::
          review_efi_delete_candidates_read_only(
              choices.value().selected_identity(),
              esp_identity.value(),
              *inspector);
      if (!reviewed_delete) {
        payload->error = format_error(
            reviewed_delete.error(), &payload->error_presentation);
        post_payload(window, kBootInspectCompleteMessage, std::move(payload));
        return;
      }
      efi_delete_plan = reviewed_delete.take_value();
    }
    payload->output = format_automatic_boot_repair_plan(
        planned, &bound_execution.value());
    if (efi_delete_plan.has_value()) {
      payload->output += ytec::winpeapp::format_reviewed_efi_delete_plan(
          efi_delete_plan.value());
    }
    payload->plan = std::move(planned);
    payload->choices = choices.take_value();
    payload->execution = bound_execution.take_value();
    payload->efi_delete_plan = std::move(efi_delete_plan);
    payload->selections = std::move(inspected);
    post_payload(window, kBootInspectCompleteMessage, std::move(payload));
  }).detach();
}

void start_boot_execute(AppState& state) {
  if (state.inspected_system_partition_creation.has_value()) {
    if (state.operation_busy ||
        state.boot_confirmation_token != L"OK" ||
        control_text(state.repair_token) != L"OK") {
      return;
    }
    const auto reviewed =
        state.inspected_system_partition_creation.value();
    auto admission_operation_id = make_operation_id(
        L"WinPE system-partition creation resume-slot admission ID");
    const std::array<std::wstring, 7U> admission_owned_fields{
        L"system-partition-creation",
        reviewed.windows_volume_name(),
        std::to_wstring(reviewed.windows_partition().number),
        std::to_wstring(reviewed.system_partition_size_bytes()),
        std::to_wstring(reviewed.reclaim_bytes()),
        std::to_wstring(reviewed.system_partition_offset_bytes()),
        std::to_wstring(static_cast<unsigned int>(reviewed.system_role())),
    };
    const std::array<std::wstring_view, 7U> admission_fields{
        admission_owned_fields[0],
        admission_owned_fields[1],
        admission_owned_fields[2],
        admission_owned_fields[3],
        admission_owned_fields[4],
        admission_owned_fields[5],
        admission_owned_fields[6],
    };
    auto resume_slot_admission = admission_operation_id
        ? ytec::winpeapp::make_winpe_resume_slot_admission_plan(
              admission_operation_id.take_value(),
              ytec::operationcore::OperationKind::boot_repair,
              std::nullopt,
              reviewed.selected_identity(),
              reviewed.reclaim_bytes(),
              admission_fields)
        : ytec::clonecore::Result<
              ytec::operationcore::OperationPlan>::failure(
              admission_operation_id.error());
    if (!guard_new_product_operation_start(
            state.window, state.repair_output, resume_slot_admission)) {
      return;
    }
    state.operation_busy = true;
    state.boot_execution_active = true;
    state.repair_step = 3;
    SetWindowTextW(
        state.repair_output,
        L"システム領域作成の実行直前再解析を行っています。\r\n"
        L"安定ディスク識別、全パーティション、Windows Volume GUID、"
        L"VDS状態、最大縮小可能量がレビューと完全一致した場合だけ、"
        L"exact NTFS縮小とESP／Active領域作成を開始します。\r\n"
        L"各段階を読戻し、失敗時は元のWindows extentへのrollbackを試みます。"
        L"この確定区間は途中取消できません。");
    show_page_controls(state);
    update_action_state(state);
    InvalidateRect(state.window, nullptr, FALSE);
    const HWND window = state.window;
    std::thread([window, reviewed]() {
      auto payload = std::make_unique<BootExecutePayload>();
      payload->system_partition_creation_stage = true;
      ThreadSleepPrevention sleep_prevention;
      if (!sleep_prevention.active()) {
        payload->output =
            L"システム領域作成中の自動スリープ防止を確立できないため、"
            L"対象ディスクを変更せずに停止しました。";
        post_payload(
            window, kBootExecuteCompleteMessage, std::move(payload));
        return;
      }
      const auto post_system_partition_creation = [&]() {
        const auto sleep_prevention_release = sleep_prevention.release();
        if (payload->success && sleep_prevention_release !=
                ytec::clonecore::SleepPreventionReleaseState::released) {
          payload->output +=
              L"\r\n\r\n自動スリープ防止の解除を確認できません。"
              L"この中間段階では完了後の電源操作を提示しません。";
        }
        post_payload(
            window, kBootExecuteCompleteMessage, std::move(payload));
      };
      auto platform = ytec::bootrepair::
          make_windows_system_partition_creation_platform();
      if (platform == nullptr) {
        payload->output =
            L"システム領域作成のMicrosoft VDS安全境界を初期化できません。";
        post_system_partition_creation();
        return;
      }
      auto report = ytec::bootrepair::execute_system_partition_creation(
          reviewed,
          ytec::clonecore::TargetConfirmation{
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
          *platform);
      payload->output = format_system_partition_creation_report(report);
      const bool committed =
          report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::committed &&
          report.confirmation_verified &&
          report.pre_mutation_revalidated && report.windows_shrunk &&
          report.shrunken_layout_verified &&
          report.system_partition_created_and_formatted &&
          report.completed_plan_verified && !report.rollback_attempted &&
          report.completed_plan.has_value();
      payload->success = committed;
      payload->partial = report.outcome == ytec::bootrepair::
          SystemPartitionCreationOutcome::rollback_incomplete;
      if (committed) {
        payload->completed_creation_plan =
            std::move(report.completed_plan);
      } else if (report.outcome == ytec::bootrepair::
                     SystemPartitionCreationOutcome::committed) {
        payload->output +=
            L"\r\n\r\n内部の必須完了フラグが一致しないため、起動修復へ進みません。";
      }
      if (report.primary_error.has_value()) {
        payload->output += L"\r\n\r\n主失敗:\r\n" + format_error(
            report.primary_error.value(), &payload->error_presentation);
      }
      if (report.rollback_error.has_value()) {
        payload->output += L"\r\n\r\nrollback失敗:\r\n" +
            format_error(report.rollback_error.value());
      }
      post_system_partition_creation();
    }).detach();
    return;
  }
  if (state.operation_busy ||
      !state.inspected_boot_plan.has_value() ||
      !state.inspected_boot_choices.has_value() ||
      !state.inspected_boot_execution.has_value() ||
      state.inspected_boot_selections.empty() ||
      (state.inspected_boot_execution->third_party_efi_delete_requested !=
       state.inspected_efi_delete_plan.has_value())) {
    return;
  }
  if (state.boot_confirmation_token != L"OK" ||
      control_text(state.repair_token) != L"OK") {
    return;
  }
  const auto reviewed_plan = state.inspected_boot_plan.value();
  const auto reviewed_choices = state.inspected_boot_choices.value();
  const auto reviewed_execution = state.inspected_boot_execution.value();
  const auto reviewed_efi_delete_plan =
      state.inspected_efi_delete_plan;
  const auto reviewed_selections = state.inspected_boot_selections;
  const auto retained = ytec::winpeapp::
      validate_reviewed_automatic_boot_repair_inspections(
          reviewed_plan,
          reviewed_choices,
          reviewed_execution,
          reviewed_selections);
  if (!retained) {
    SetWindowTextW(
        state.repair_output,
        L"レビュー済み起動修復計画の内部照合に失敗しました。"
        L"自動解析をやり直してください。");
    invalidate_boot_review(state);
    update_action_state(state);
    return;
  }
  const std::wstring transaction_token =
      ytec::bootrepair::make_boot_repair_confirmation_token(
          reviewed_choices.selected_identity(), reviewed_choices.firmware());
  std::wstring windows_binding;
  for (std::size_t index = 0U;
       index < reviewed_execution.requests_in_boot_priority.size();
       ++index) {
    const auto& request =
        reviewed_execution.requests_in_boot_priority[index];
    windows_binding +=
        std::to_wstring(
            reviewed_execution
                .windows_partition_numbers_in_boot_priority[index]) +
        L":" +
        request.windows_root + L";";
  }
  auto admission_operation_id = make_operation_id(
      L"WinPE automatic boot-repair resume-slot admission ID");
  const std::array<std::wstring, 9U> admission_owned_fields{
      L"automatic-boot-repair",
      transaction_token,
      L"windows-count=" +
          std::to_wstring(reviewed_execution.requests_in_boot_priority.size()),
      L"firmware=" + std::to_wstring(
          static_cast<unsigned int>(reviewed_choices.firmware())),
      L"store-policy=" + std::to_wstring(
          static_cast<unsigned int>(reviewed_choices.bcd_store_policy())),
      L"third-party-policy=" + std::to_wstring(
          static_cast<unsigned int>(
              reviewed_choices.third_party_efi_policy())),
      L"nvram-policy=" + std::to_wstring(
          static_cast<unsigned int>(reviewed_choices.nvram_policy())),
      reviewed_efi_delete_plan.has_value()
          ? L"reviewed-efi-delete"
          : L"preserve-or-not-applicable-efi",
      windows_binding,
  };
  const std::array<std::wstring_view, 9U> admission_fields{
      admission_owned_fields[0],
      admission_owned_fields[1],
      admission_owned_fields[2],
      admission_owned_fields[3],
      admission_owned_fields[4],
      admission_owned_fields[5],
      admission_owned_fields[6],
      admission_owned_fields[7],
      admission_owned_fields[8],
  };
  auto resume_slot_admission = admission_operation_id
      ? ytec::winpeapp::make_winpe_resume_slot_admission_plan(
            admission_operation_id.take_value(),
            ytec::operationcore::OperationKind::boot_repair,
            std::nullopt,
            reviewed_choices.selected_identity(),
            reviewed_choices.selected_identity().size_bytes,
            admission_fields)
      : ytec::clonecore::Result<ytec::operationcore::OperationPlan>::failure(
            admission_operation_id.error());
  if (!guard_new_product_operation_start(
          state.window, state.repair_output, resume_slot_admission)) {
    return;
  }
  const std::uint64_t completion_power_binding =
      ytec::winpeapp::take_winpe_completion_power_operation_binding(
          state.next_completion_power_operation_binding);
  const std::wstring completion_power_target =
      format_completion_disk_target(reviewed_choices.selected_identity());
  state.operation_busy = true;
  state.boot_execution_active = true;
  state.repair_step = 3;
  SetWindowTextW(
      state.repair_output,
      L"実行直前に対象ディスクを再解析し、レビュー済みの全レイアウト、"
      L"Windows、システム領域を再照合しています。\r\n"
      L"完全一致した場合だけ、Microsoft署名済みBCDBoot /cの"
      L"退避・検証・ロールバック対応トランザクションを実行します。\r\n"
      L"確認済みWinre.wimがある場合は、BCD成功後に同一ファイルを再lockして"
      L"署名済みREAgentCで登録・再診断します。\r\n"
      L"BCD／WinRE確定処理は安全のため途中取消できません。");
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  const HWND window = state.window;
  std::thread([
      window,
       reviewed_choices,
       reviewed_execution,
       reviewed_efi_delete_plan,
       completion_power_binding,
       completion_power_target,
       transaction_token]() {
    auto payload = std::make_unique<BootExecutePayload>();
    payload->completion_power_target = completion_power_target;
    ThreadSleepPrevention sleep_prevention;
    if (!sleep_prevention.active()) {
      payload->output =
          L"起動修復中の自動スリープ防止を確立できないため、"
          L"対象ディスクを変更せずに停止しました。";
      post_payload(window, kBootExecuteCompleteMessage, std::move(payload));
      return;
    }
    const auto post_boot_completion = [&]() {
      const auto sleep_prevention_release = sleep_prevention.release();
      payload->completion_power_proof =
          ytec::winpeapp::make_winpe_completion_power_proof(
              ytec::winpeapp::WinPeCompletionPowerOperation::boot_repair,
              completion_operation_outcome(
                  payload->success,
                  payload->success && payload->partial),
              payload->success && !payload->partial,
              sleep_prevention_release,
              completion_power_binding);
      if (payload->success && sleep_prevention_release !=
              ytec::clonecore::SleepPreventionReleaseState::released) {
        payload->output +=
            L"\r\n\r\n自動スリープ防止の解除を確認できないため、"
            L"完了後の再起動／シャットダウンは提示しません。";
      }
      post_payload(window, kBootExecuteCompleteMessage, std::move(payload));
    };
    auto planner =
        ytec::bootrepair::make_windows_automatic_boot_repair_plan_service();
    if (planner == nullptr) {
      payload->output = L"実行直前の自動再解析を初期化できません。";
      post_boot_completion();
      return;
    }
    auto observed_plan = planner->plan(reviewed_choices.selected_identity());
    if (!observed_plan) {
      payload->output = format_error(
          observed_plan.error(), &payload->error_presentation);
      post_boot_completion();
      return;
    }
    auto observed_choices = ytec::bootrepair::
        revalidate_automatic_boot_repair_choices(
            reviewed_choices, observed_plan.value());
    if (!observed_choices) {
      payload->output = format_error(
          observed_choices.error(), &payload->error_presentation);
      post_boot_completion();
      return;
    }
    auto observed_execution = ytec::winpeapp::
        build_executable_reviewed_automatic_boot_repair(
            observed_choices.value());
    if (!observed_execution) {
      payload->output = format_error(
          observed_execution.error(), &payload->error_presentation);
      post_boot_completion();
      return;
    }
    auto bound_execution = carry_reviewed_boot_repair_winre_images(
        observed_execution.take_value(), reviewed_execution);
    if (!bound_execution) {
      payload->output = format_error(
          bound_execution.error(), &payload->error_presentation);
      post_boot_completion();
      return;
    }
    auto provider =
        ytec::diskmodel::make_windows_disk_inventory_provider();
    auto service = provider == nullptr
        ? nullptr
        : ytec::bootrepair::make_windows_standalone_boot_repair_service(
              *provider);
    if (service == nullptr) {
      payload->output = L"起動修復の安全実行境界を初期化できません。";
      post_boot_completion();
      return;
    }
    std::vector<ytec::bootrepair::BootRepairTargetSelection> inspected;
    inspected.reserve(
        bound_execution.value().requests_in_boot_priority.size());
    for (const auto& request :
         bound_execution.value().requests_in_boot_priority) {
      auto one = service->inspect(request);
      if (!one) {
        payload->output = format_error(
            one.error(), &payload->error_presentation);
        post_boot_completion();
        return;
      }
      inspected.push_back(one.take_value());
    }
    const auto inspection_status = ytec::winpeapp::
        validate_reviewed_automatic_boot_repair_inspections(
            observed_plan.value(),
            observed_choices.value(),
            bound_execution.value(),
            inspected);
    if (!inspection_status) {
      payload->output = format_error(
          inspection_status.error(), &payload->error_presentation);
      post_boot_completion();
      return;
    }
    const bool partial =
        bound_execution.value().normal_boot_only_partial;
    const bool preserved_third_party =
        bound_execution.value().third_party_efi_preserved;
    const bool deleted_third_party =
        bound_execution.value().third_party_efi_delete_requested;
    const bool repair_current_pc_nvram =
        bound_execution.value().repair_current_pc_nvram;
    const auto reviewed_nvram_esp =
        observed_choices.value().system_partition().partition;
    const auto firmware = observed_choices.value().firmware();
    std::wstring efi_transaction_summary;
    auto result = [&]() -> ytec::clonecore::Result<
        ytec::bootrepair::MultiWindowsStandaloneBootRepairReport> {
      if (!bound_execution.value().third_party_efi_delete_requested) {
        return service->execute_multi_windows(
            ytec::bootrepair::
                MultiWindowsStandaloneBootRepairExecutionRequest{
                .targets_in_boot_priority =
                    bound_execution.value().requests_in_boot_priority,
                .expected_in_boot_priority = inspected,
                .confirmation =
                    ytec::clonecore::TargetConfirmation{
                        .first_step_acknowledged = true,
                        .typed_token = transaction_token,
                    },
            });
      }
      if (!reviewed_efi_delete_plan.has_value()) {
        return ytec::clonecore::Result<
            ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
            failure(boot_repair_winre_error(
                ytec::clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"第三者EFI削除review保持",
                L"専用immutable manifestが保持されていません"));
      }

      auto mount_api =
          ytec::bootrepair::make_windows_system_volume_mount_api();
      std::optional<ytec::bootrepair::TemporarySystemVolumeMount>
          temporary_mount;
      std::wstring system_root = bound_execution.value()
          .requests_in_boot_priority.front().system_root;
      if (bound_execution.value().temporary_system_mount_required) {
        auto volumes = ytec::bootrepair::
            enumerate_windows_boot_volumes_read_only();
        const DWORD drive_mask = GetLogicalDrives();
        std::wstring unavailable;
        for (std::size_t index = 0U; index < 26U; ++index) {
          if ((drive_mask & (1UL << index)) != 0U) {
            unavailable.push_back(
                static_cast<wchar_t>(L'A' + index));
          }
        }
        auto mount_plan = volumes
            ? ytec::bootrepair::plan_temporary_system_volume_mount(
                  observed_plan.value().selected_disk,
                  firmware,
                  volumes.value(),
                  unavailable)
            : ytec::clonecore::Result<
                  ytec::bootrepair::TemporarySystemVolumeMountPlan>::
                  failure(volumes.error());
        if (!mount_plan || mount_api == nullptr) {
          return ytec::clonecore::Result<
              ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
              failure(
                  !mount_plan
                      ? mount_plan.error()
                      : boot_repair_winre_error(
                            ytec::clonecore::ErrorCode::internal_error,
                            ERROR_NOT_ENOUGH_MEMORY,
                            L"第三者EFI削除ESP一時mount",
                            L"system volume mount APIを初期化できません"));
        }
        auto mounted = ytec::bootrepair::TemporarySystemVolumeMount::acquire(
            mount_plan.value(), *mount_api);
        if (!mounted) {
          return ytec::clonecore::Result<
              ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
              failure(mounted.error());
        }
        temporary_mount.emplace(mounted.take_value());
        system_root = temporary_mount->root();
      }

      std::vector<ytec::bootrepair::BcdBootRequest> bcd_requests;
      bcd_requests.reserve(
          bound_execution.value().requests_in_boot_priority.size());
      for (const auto& request :
           bound_execution.value().requests_in_boot_priority) {
        bcd_requests.push_back(ytec::bootrepair::BcdBootRequest{
            .target_windows_directory = request.windows_root + L"Windows",
            .target_system_partition_root = system_root,
            .firmware = request.firmware,
            .store_policy = request.store_policy,
        });
      }
      auto platform = ytec::bootrepair::
          make_windows_efi_delete_transaction_platform(
              std::move(bcd_requests));
      if (!platform) {
        return ytec::clonecore::Result<
            ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
            failure(platform.error());
      }
      auto transaction = ytec::bootrepair::execute_efi_delete_transaction(
          reviewed_efi_delete_plan.value(),
          ytec::bootrepair::EfiDeleteConfirmation{
              .destructive_warning_acknowledged = true,
              .typed_token = L"OK",
          },
          *platform.value());
      efi_transaction_summary = ytec::winpeapp::
          format_efi_delete_transaction_report(transaction);

      ytec::clonecore::Status released = ytec::clonecore::success_status();
      if (temporary_mount.has_value()) {
        released = temporary_mount->release();
        if (!released) {
          efi_transaction_summary +=
              L"\r\nESP一時割当の解除を完全確認できません: " +
              format_error(
                  released.error(), &payload->error_presentation);
        }
      }
      if (transaction.outcome !=
          ytec::bootrepair::EfiDeleteTransactionOutcome::committed) {
        return ytec::clonecore::Result<
            ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
            failure(
                transaction.primary_error.value_or(
                    boot_repair_winre_error(
                        ytec::clonecore::ErrorCode::verification_failed,
                        ERROR_GEN_FAILURE,
                        L"第三者EFI削除transaction",
                        L"結果分類がCOMMITTEDではありません")));
      }
      if (!released) {
        return ytec::clonecore::Result<
            ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
            failure(released.error());
      }
      const auto& bcd_report = platform.value()->verified_bcd_report();
      if (!bcd_report.has_value()) {
        return ytec::clonecore::Result<
            ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
            failure(boot_repair_winre_error(
                ytec::clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"第三者EFI削除BCD report",
                L"commit済みtransactionにBCD readback reportがありません"));
      }
      return ytec::clonecore::Result<
          ytec::bootrepair::MultiWindowsStandaloneBootRepairReport>::
          success(
              ytec::bootrepair::MultiWindowsStandaloneBootRepairReport{
                  .repaired_in_boot_priority = inspected,
                  .bcdboot = bcd_report.value(),
                  .boot_store_verified = true,
                  .system_partition_temporarily_mounted =
                      temporary_mount.has_value(),
                  .temporary_mount_released = true,
                  .efi_ownership_revalidated = true,
                  .nvram_unchanged = true,
              });
    }();
    if (result) {
      const bool every_registration_verified = std::all_of(
          result.value().bcdboot.windows_registrations.begin(),
          result.value().bcdboot.windows_registrations.end(),
          [](const auto& registration) {
            return registration.microsoft_signature_verified &&
                registration.exit_code == 0U &&
                registration.fresh_store_verified;
          });
      payload->success =
          !result.value().bcdboot.windows_registrations.empty() &&
          every_registration_verified &&
          result.value().bcdboot.fresh_store_verified &&
          result.value().boot_store_verified &&
          result.value().efi_ownership_revalidated &&
          result.value().nvram_unchanged &&
          (!result.value().system_partition_temporarily_mounted ||
           result.value().temporary_mount_released);
      if (payload->success) {
        bool winre_partial = partial;
        std::size_t winre_existing_count{};
        std::size_t winre_registered_count{};
        std::wstring winre_failure_detail;
        const auto registration_count = static_cast<std::size_t>(
            std::count_if(
                bound_execution.value()
                    .winre_actions_in_boot_priority.begin(),
                bound_execution.value()
                    .winre_actions_in_boot_priority.end(),
                [](const auto& action) {
                  return action.disposition == ytec::bootrepair::
                      AutomaticWinReRepairDisposition::
                          register_verified_windows_image;
                }));
        for (const auto& action :
             bound_execution.value().winre_actions_in_boot_priority) {
          if (action.disposition == ytec::bootrepair::
                  AutomaticWinReRepairDisposition::
                      verify_existing_registration) {
            ++winre_existing_count;
          }
        }

        if (registration_count != 0U) {
          auto system_directory = query_boot_repair_system_directory();
          auto trust =
              ytec::bootrepair::make_windows_authenticode_verifier();
          auto process = ytec::bootrepair::make_windows_process_runner();
          auto locker = ytec::bootrepair::
              make_windows_winre_registration_image_locker();
          auto diagnostic =
              ytec::bootrepair::make_windows_winre_diagnostic_service();
          if (!system_directory || trust == nullptr || process == nullptr ||
              locker == nullptr || diagnostic == nullptr) {
            winre_partial = true;
            winre_failure_detail = !system_directory
                ? format_error(
                      system_directory.error(),
                      &payload->error_presentation)
                : L"WinRE再登録の署名検証、固定引数process、画像lock、"
                  L"または再診断基盤を初期化できませんでした。";
          } else {
            for (const auto& action :
                 bound_execution.value().winre_actions_in_boot_priority) {
              if (action.disposition != ytec::bootrepair::
                      AutomaticWinReRepairDisposition::
                          register_verified_windows_image) {
                continue;
              }
              WinPeAutomaticBootRepairWinReTargetGuard guard(
                  observed_plan.value(),
                  action.windows_partition_number,
                  *provider);
              auto registered = ytec::bootrepair::
                  execute_winre_registration_transaction(
                      ytec::bootrepair::WinReRegistrationRequest{
                          .intent = ytec::bootrepair::
                              WinReRegistrationIntent::
                                  register_verified_image,
                          .prior_state_origin = ytec::bootrepair::
                              WinReRegistrationPriorStateOrigin::
                                  existing_target,
                          .offline_windows_directory =
                              action.offline_windows_directory,
                          .trusted_system_directory =
                              system_directory.value(),
                          .candidate_directory =
                              action.candidate_directory,
                          .rollback_candidate_directory = {},
                          .expected_target_disk_number =
                              observed_plan.value().selected_disk.disk_number,
                          .expected_target_partition_number =
                              action.expected_target_partition_number,
                          .expected_registered_path_kind =
                              action.expected_registered_path_kind,
                          .reviewed_candidate = action.reviewed_candidate,
                          .prior_diagnostic = action.prior_diagnostic,
                          .reviewed_rollback_image = std::nullopt,
                      },
                      *trust,
                      *process,
                      *locker,
                      guard,
                      *diagnostic);
              if (!registered) {
                winre_partial = true;
                if (winre_failure_detail.empty()) {
                  winre_failure_detail = format_error(
                      registered.error(), &payload->error_presentation);
                }
                break;
              }
              if (registered.value().outcome == ytec::bootrepair::
                      WinReRegistrationOutcome::completed &&
                  registered.value().candidate_locked &&
                  registered.value().reagentc_signature_verified &&
                  registered.value().set_reimage_completed &&
                  registered.value().enable_completed &&
                  registered.value().registration_verified &&
                  !registered.value().rollback_attempted) {
                ++winre_registered_count;
                continue;
              }
              winre_partial = true;
              if (winre_failure_detail.empty()) {
                if (registered.value().outcome == ytec::bootrepair::
                        WinReRegistrationOutcome::
                            failed_rollback_incomplete) {
                  winre_failure_detail =
                      L"WinRE再登録に失敗し、変更前の未登録状態へ戻ったことも完全確認できませんでした。";
                } else if (registered.value().primary_failure.has_value()) {
                  winre_failure_detail = format_error(
                      *registered.value().primary_failure,
                      &payload->error_presentation);
                } else {
                  winre_failure_detail =
                      L"WinRE再登録を完了確認できなかったため、通常起動だけの部分修復です。";
                }
              }
              break;
            }
          }
        }
        bool nvram_partial = false;
        bool nvram_verified = false;
        std::wstring nvram_failure_detail;
        if (repair_current_pc_nvram) {
          auto nvram_platform = ytec::bootrepair::
              make_windows_current_pc_nvram_repair_platform(
                  [expected_plan = observed_plan.value(),
                   expected_esp = reviewed_nvram_esp,
                   inventory = provider.get()](
                      const ytec::clonecore::StableDiskIdentity& identity,
                      const ytec::diskmodel::PartitionInfo& esp) {
                    if (inventory == nullptr) {
                      return ytec::clonecore::Status::failure(
                          boot_repair_winre_error(
                              ytec::clonecore::ErrorCode::internal_error,
                              ERROR_INVALID_HANDLE,
                              L"UEFI NVRAM inventory",
                              L"対象再識別providerが失われました"));
                    }
                    return revalidate_current_pc_nvram_target(
                        identity, esp, expected_plan, expected_esp, *inventory);
                  });
          if (nvram_platform == nullptr) {
            nvram_partial = true;
            nvram_failure_detail =
                L"NVRAM条件付き更新基盤を初期化できませんでした。";
          } else {
            auto nvram = ytec::bootrepair::execute_current_pc_nvram_repair(
                ytec::bootrepair::CurrentPcNvramRepairRequest{
                    .expected_disk = observed_choices.value().selected_identity(),
                    .expected_esp = reviewed_nvram_esp,
                    .logical_sector_size =
                        observed_plan.value().selected_disk.logical_sector_size,
                    .explicitly_for_current_pc = true,
                    .confirmation = ytec::clonecore::TargetConfirmation{
                        .first_step_acknowledged = true,
                        .typed_token = L"OK",
                    },
                },
                *nvram_platform);
            nvram_verified = nvram.has_value() &&
                nvram.value().prior_boot_order_preserved &&
                nvram.value().exact_target_verified;
            if (!nvram_verified) {
              nvram_partial = true;
              nvram_failure_detail = nvram
                  ? L"NVRAM更新結果の対象ESP／BootOrder完全検証を確認できませんでした。"
                  : format_error(
                        nvram.error(), &payload->error_presentation);
            }
          }
        }
        payload->partial = winre_partial || nvram_partial;
        payload->success = payload->success && !nvram_partial;
        std::wstring winre_summary;
        if (winre_registered_count != 0U) {
          winre_summary += L"・WinRE再登録・再診断: " +
              std::to_wstring(winre_registered_count) + L"件 完了\r\n";
        }
        if (winre_existing_count != 0U) {
          winre_summary += L"・WinRE既存登録: " +
              std::to_wstring(winre_existing_count) +
              L"件 変更なし\r\n";
        }
        if (winre_partial) {
          winre_summary +=
              L"・WinRE: 未修復または完了未確認（通常起動のみ部分完了）\r\n";
          if (!winre_failure_detail.empty()) {
            winre_summary += L"・WinRE部分修復の根拠: " +
                winre_failure_detail + L"\r\n";
          }
        }
        std::wstring nvram_summary;
        if (!repair_current_pc_nvram) {
          nvram_summary = L"・UEFI NVRAM: 明示選択どおり変更なし\r\n";
        } else if (nvram_verified) {
          nvram_summary =
              L"・UEFI NVRAM: 対象ESPのWindows Boot Managerと既存BootOrder順を完全確認済み\r\n";
        } else {
          nvram_summary =
              L"・UEFI NVRAM: 修復を完了確認できません（BCDは完了）\r\n";
          if (!nvram_failure_detail.empty()) {
            nvram_summary += L"・NVRAM部分修復の根拠: " +
                nvram_failure_detail + L"\r\n";
          }
        }
        payload->output =
            std::wstring(payload->partial
                ? L"通常起動の部分修復が完了しました。未確認項目は完全完了扱いにしていません。\r\n"
                : L"起動修復の安全トランザクションが完了しました。\r\n") +
            L"・Microsoft署名: 確認済み\r\n"
            L"・BCDBoot /c＋表示順のWindows登録: 正常終了\r\n"
            L"・登録Windows数: " +
            std::to_wstring(
                result.value().bcdboot.windows_registrations.size()) +
            L"\r\n"
             L"・新規BCDストア: 通常ファイル存在確認済み\r\n"
             L"・起動ストア: 通常ファイル存在確認済み\r\n" +
            std::wstring(
                firmware == ytec::bootrepair::BcdBootFirmware::uefi
                    ? L"・EFI所有権: 実行直前に再確認済み\r\n"
                    : L"・EFI所有権: BIOSのため対象外\r\n") +
            std::wstring(
                preserved_third_party
                    ? L"・第三者EFI: 保持（削除・改名なし）\r\n"
                    : deleted_third_party
                        ? L"・第三者EFI: 専用transactionで削除・readback済み\r\n"
                        : L"") +
            nvram_summary +
            winre_summary +
            std::wstring(
                result.value().system_partition_temporarily_mounted
                    ? L"・一時割り当て: 解除確認済み\r\n"
                    : L"・一時割り当て: 不要\r\n") +
            L"\r\n"
            L"起動成功を確認した表示ではありません。"
            L"USB/ISOを取り外し、修復したディスクから起動確認してください。";
        payload->output += efi_transaction_summary;
      } else {
        payload->output =
            L"BCDBootから結果は返りましたが、署名、新規BCD、"
            L"通常ファイル存在、EFI再照合、NVRAM非変更、"
            L"または一時割り当て解除の必須確認を"
            L"すべて満たしませんでした。完了扱いにしません。";
      }
    } else {
      payload->output = format_error(
          result.error(), &payload->error_presentation);
      payload->output += efi_transaction_summary;
    }
    post_boot_completion();
  }).detach();
}

void cancel_boot_review(AppState& state) {
  if (state.operation_busy ||
      (!state.inspected_system_partition_creation.has_value() &&
       !state.inspected_boot_execution.has_value())) {
    return;
  }
  invalidate_boot_review(state);
  SetWindowTextW(
      state.repair_output,
      L"確認済みのシステム領域作成／起動修復計画を破棄しました。"
      L"ディスクは変更していません。\r\n"
      L"再開する場合は、対象ディスクを選んで自動解析をやり直してください。");
  update_action_state(state);
  InvalidateRect(state.window, nullptr, FALSE);
  SetFocus(state.repair_disk);
}

void paint_sidebar(const AppState& state, HDC dc, const RECT& client) {
  RECT sidebar{0, 0, 230, client.bottom};
  SetDCBrushColor(dc, kSidebar);
  FillRect(dc, &sidebar, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

  RECT brand{20, 24, 210, 62};
  draw_text(
      dc,
      L"Y-TEC",
      brand,
      state.heading_font,
      RGB(255, 255, 255),
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT name{20, 59, 214, 91};
  draw_text(
      dc,
      L"Tsumugi Drive",
      name,
      state.body_font,
      RGB(215, 231, 235),
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  draw_strands(dc, 22, 112, 170);
  RECT concept_bounds{
      22, kSidebarConceptTop, 210, kSidebarConceptBottom};
  draw_text(
      dc,
      L"3つの工程を、ひとつに",
      concept_bounds,
      state.small_font,
      RGB(161, 179, 191),
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (client.bottom < 640) {
    RECT compact{20, 424, 208, client.bottom - 6};
    draw_text(
        dc,
        L"起動環境モード・通信なし\r\n"
        L"不明な対象は実行しません。\r\n"
        L"コピー元は変更しません。",
        compact,
        state.small_font,
        RGB(161, 179, 191),
        DT_LEFT | DT_TOP);
  } else {
    RECT mode{20, kSidebarModeTop, 208, kSidebarModeTop + 52};
    draw_text(
        dc,
        L"起動環境モード\r\nネットワーク通信なし",
        mode,
        state.small_font,
        RGB(161, 179, 191),
        DT_LEFT | DT_TOP);
    RECT safety{20, client.bottom - 96, 208, client.bottom - 24};
    draw_text(
        dc,
        L"不明な対象は実行しません。\r\nコピー元は変更しません。",
        safety,
        state.small_font,
        RGB(161, 179, 191),
        DT_LEFT | DT_TOP);
  }
}

void paint_header(const AppState& state, HDC dc, const RECT& client) {
  RECT header{230, 0, client.right, 88};
  FillRect(dc, &header, state.card_brush);
  const wchar_t* title = L"ドライブをクローン";
  const wchar_t* subtitle =
      L"このPEでコピー元とコピー先を直接選択して実行します";
  if (state.page == Page::image_create) {
    title = L"イメージを作成";
    subtitle =
        L"通常／救出と .tsumugi 保存先を選び、このPE内で直接作成します";
  } else if (state.page == Page::image_restore) {
    title = L"イメージを復元";
    subtitle =
        L".tsumugi と復元先を選び、このPE内で完全検証して直接復元します";
  } else if (state.page == Page::boot_repair) {
    title = L"起動修復だけを行う";
    subtitle =
        L"対象ディスクだけを選び、Windowsと起動方式を自動判定します";
  } else if (state.page == Page::disk_diagnostics) {
    title = L"接続ディスクを確認";
    subtitle =
        L"モデル・容量・形式・パーティションを読み取り専用で表示します";
  }
  RECT title_bounds{260, 18, client.right - 28, 49};
  draw_text(
      dc,
      title,
      title_bounds,
      state.heading_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT subtitle_bounds{260, 51, client.right - 28, 78};
  draw_text(
      dc,
      subtitle,
      subtitle_bounds,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_status_banner(const AppState& state, HDC dc) {
  const RECT content = content_rect(state);
  const COLORREF color =
      state.dashboard.readiness == ytec::winpeapp::DashboardReadiness::ready
      ? kSafeGreen
      : state.dashboard.readiness ==
                ytec::winpeapp::DashboardReadiness::scanning
      ? kTsumugiBlue
      : state.dashboard.readiness ==
                ytec::winpeapp::DashboardReadiness::warning
      ? kWarning
      : kDanger;
  RECT banner{
      content.left + 16,
      102,
      content.right - 16,
      150};
  fill_rounded_rect(dc, banner, RGB(255, 255, 255), color);
  RECT headline{banner.left + 16, banner.top + 7, banner.right - 12, banner.top + 27};
  draw_text(
      dc,
      state.dashboard.headline,
      headline,
      state.body_font,
      color,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT guidance{banner.left + 16, banner.top + 25, banner.right - 12, banner.bottom - 3};
  draw_text(
      dc,
      state.dashboard.guidance,
      guidance,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paint_clone_page(const AppState& state, HDC dc) {
  const RECT content = content_rect(state);
  const CloneRoute route = selected_clone_route(state);
  const std::wstring target_label(L"コピー先");
  const std::wstring operation_label = route == CloneRoute::rescue
      ? L"救出クローン"
      : route == CloneRoute::mbr_to_gpt
      ? L"MBR→GPT移行"
      : route == CloneRoute::shrink
      ? L"縮小移行"
      : L"クローン";
  draw_stepper(
      state,
      dc,
      {L"対象選択", L"読取り確認", L"最終確認", L"実行"},
      state.clone_step);
  RECT path_card{content.left + 8, 168, content.right - 8, 260};
  fill_rounded_rect(dc, path_card, kCard, kBorder);
  const int selection_width = path_card.right - path_card.left - 196;
  const int selection_field_width = (std::max)(220, selection_width / 2);
  RECT label{
      path_card.left + 16,
      kClonePathLabelTop,
      path_card.left + 16 + selection_field_width,
      kClonePathLabelBottom};
  draw_text(
      dc,
      L"コピー元",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  label.left += selection_field_width + 12;
  label.right += selection_field_width + 12;
  draw_text(
      dc,
      L"コピー先（全内容を消去）",
      label,
      state.small_font,
      kDanger,
      DT_LEFT | DT_TOP | DT_SINGLELINE);

  RECT action_card{content.left + 8, 276, content.right - 8, 430};
  action_card.bottom = 464;
  fill_rounded_rect(dc, action_card, kCard, kBorder);
  if (state.clone_progress_active) {
    const auto progress = ytec::winpeapp::build_operation_progress_view(
        state.clone_progress, state.clone_elapsed);
    RECT stage{
        action_card.left + 16,
        action_card.top + 11,
        action_card.right - 164,
        action_card.top + 35};
    draw_text(
        dc,
        progress.stage_label,
        stage,
        state.body_font,
        progress.cancellation_allowed ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT percentage{
        action_card.right - 156,
        action_card.top + 11,
        action_card.right - 16,
        action_card.top + 35};
    draw_text(
        dc,
        L"検証済み " + progress.percentage_label,
        percentage,
        state.body_font,
        kSafeGreen,
        DT_RIGHT | DT_TOP | DT_SINGLELINE);

    RECT partition{
        action_card.left + 16,
        action_card.top + 34,
        action_card.right - 16,
        action_card.top + 54};
    draw_text(
        dc,
        L"現在: " + progress.partition_label,
        partition,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT track{
        action_card.left + 16,
        action_card.top + 58,
        action_card.right - 16,
        action_card.top + 74};
    fill_rounded_rect(dc, track, RGB(226, 233, 237), RGB(226, 233, 237));
    RECT fill = track;
    fill.right =
        fill.left + static_cast<int>(
                        static_cast<double>(fill.right - fill.left) *
                        progress.fraction);
    if (fill.right > fill.left) {
      fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue);
    }

    const int inner_left = action_card.left + 16;
    const int inner_right = action_card.right - 16;
    const int inner_width = inner_right - inner_left;
    const int eta_width = (std::max)(174, inner_width * 24 / 100);
    const int gap = 8;
    const int transfer_width = inner_width - eta_width - gap;
    const int metric_width = (transfer_width - gap * 2) / 3;
    const int metric_top = action_card.top + 83;
    const int metric_bottom = action_card.top + 132;
    const RECT read_tile{
        inner_left,
        metric_top,
        inner_left + metric_width,
        metric_bottom};
    const RECT write_tile{
        read_tile.right + gap,
        metric_top,
        read_tile.right + gap + metric_width,
        metric_bottom};
    const RECT verify_tile{
        write_tile.right + gap,
        metric_top,
        inner_left + transfer_width,
        metric_bottom};
    const RECT eta_tile{
        inner_left + transfer_width + gap,
        metric_top,
        inner_right,
        metric_bottom};
    draw_metric_tile(
        state,
        dc,
        read_tile,
        L"読込",
        progress.read_label,
        kTsumugiBlue,
        RGB(242, 249, 250));
    draw_metric_tile(
        state,
        dc,
        write_tile,
        L"書込",
        progress.write_label,
        kTsumugiPurple,
        RGB(247, 244, 251));
    draw_metric_tile(
        state,
        dc,
        verify_tile,
        L"検証",
        progress.verified_label,
        kSafeGreen,
        RGB(243, 250, 247));
    draw_metric_tile(
        state,
        dc,
        eta_tile,
        L"残り時間",
        progress.remaining_label,
        kWarning,
        RGB(253, 248, 240));

    RECT cancellation{
        action_card.left + 16,
        action_card.top + 141,
        action_card.right - 188,
        action_card.bottom - 8};
    draw_text(
        dc,
        L"速度 " + progress.speed_label +
            L"  •  経過 " + progress.elapsed_label +
            (progress.cancellation_allowed
                 ? L"  •  安全な境界で取消可能"
                 : L"  •  整合性保護のため取消不可"),
        cancellation,
        state.small_font,
        progress.cancellation_allowed ? kMuted : kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else if (state.clone_execution_ready) {
    RECT title{
        action_card.left + 16,
        action_card.top + 11,
        action_card.right - 16,
        action_card.top + 35};
    draw_text(
        dc,
        L"最終確認 — 表示された" + target_label +
            L"だけが完全消去されます",
        title,
        state.body_font,
        kDanger,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT token{
        action_card.left + 16,
        action_card.top + 39,
        action_card.right - 16,
        action_card.top + 62};
    draw_text(
        dc,
        L"確認語: " + state.clone_confirmation_token,
        token,
        state.mono_font,
        kInk,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT guidance{
        action_card.left + 16,
        action_card.top + 94,
        action_card.right - 210,
        action_card.bottom - 8};
    draw_text(
        dc,
        L"チェック後、確認語を入力すると" + operation_label +
            L"ボタンが有効になります。",
        guidance,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
  } else {
    RECT title{
        action_card.left + 16,
        action_card.top + 13,
        action_card.right - 16,
        action_card.top + 38};
    draw_text(
        dc,
        state.operation_busy
            ? L"読み取り専用で安全確認しています"
            : L"コピー元とコピー先を選び、安全確認を開始してください",
        title,
        state.body_font,
        state.operation_busy ? kTsumugiBlue : kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT guidance{
        action_card.left + 16,
        action_card.top + (state.operation_busy ? 48 : 88),
        action_card.right - 16,
        action_card.bottom - 12};
    draw_text(
        dc,
        state.operation_busy
            ? L"安全確認後は、このPE内でそのまま直接実行します。"
            : route == CloneRoute::rescue
            ? L"救出モードは縮小・形式変換・起動修復を行わず、結果を常に「一部欠損の可能性あり」として扱います。"
            : route == CloneRoute::mbr_to_gpt
            ? L"MBR→GPTは別ディスクだけを対象にし、1～3基本領域と一意なActive領域を確認してからコピー先だけを変換します。"
            : route == CloneRoute::shrink
            ? L"縮小移行はWinPE専用です。GPT/512B/完全復号済みNTFSを再構築し、"
              L"未対応FSのデータ領域は元サイズRAWで保持します。4Knや安全に特定・検証できない構成は停止します。"
            : L"通常モードは読取りエラーで停止します。読取り異常が疑われる場合だけ救出モードを選択してください。",
        guidance,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
  }

  if (content.bottom < 576) {
    return;
  }
  RECT result_card{content.left + 8, 476, content.right - 8, content.bottom};
  fill_rounded_rect(dc, result_card, kCard, kBorder);
  RECT result_title{
      result_card.left + 16,
      result_card.top + 11,
      result_card.right - 16,
      result_card.top + 35};
  draw_text(
      dc,
      state.clone_progress_active
          ? operation_label + L"の状態"
          : state.operation_busy
          ? L"安全確認を実行中"
          : L"確認・実行結果",
      result_title,
      state.body_font,
      state.operation_busy ? kTsumugiBlue : kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_image_create_page(const AppState& state, HDC dc) {
  RECT client{};
  GetClientRect(state.window, &client);
  const RECT content = content_rect(state);
  const auto layout = ytec::winpeapp::build_winpe_image_create_layout(
      client.right, client.bottom);
  draw_stepper(
      state,
      dc,
      {L"作成元", L"内容確認", L"最終確認", L"作成"},
      state.image_step);

  RECT selection_card{content.left + 8, 168, content.right - 8, 378};
  fill_rounded_rect(dc, selection_card, kCard, kBorder);
  RECT label{
      layout.source.left,
      178,
      layout.source.right,
      198};
  draw_text(
      dc,
      L"作成元ディスク（実行時にread-only固定）",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  label = RECT{
      layout.destination.left,
      238,
      layout.destination.right,
      258};
  draw_text(
      dc,
      L"保存先（NTFS / exFAT の単一 .tsumugi）",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  RECT action_card{content.left + 8, 384, content.right - 8, 484};
  fill_rounded_rect(dc, action_card, kCard, kBorder);
  if (state.image_progress_active) {
    const auto progress = ytec::winpeapp::build_operation_progress_view(
        state.image_progress, state.image_elapsed);
    RECT stage{
        action_card.left + 16,
        394,
        action_card.right - 164,
        416};
    draw_text(
        dc,
        progress.stage_label,
        stage,
        state.body_font,
        progress.cancellation_allowed ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT percentage{
        action_card.right - 156,
        394,
        action_card.right - 16,
        416};
    draw_text(
        dc,
        L"検証済み " + progress.percentage_label,
        percentage,
        state.small_font,
        kSafeGreen,
        DT_RIGHT | DT_TOP | DT_SINGLELINE);
    RECT partition{
        action_card.left + 16,
        418,
        action_card.right - 16,
        435};
    draw_text(
        dc,
        L"現在: " + progress.partition_label,
        partition,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT track{
        action_card.left + 16,
        438,
        action_card.right - 16,
        450};
    fill_rounded_rect(dc, track, RGB(226, 233, 237), RGB(226, 233, 237));
    RECT fill = track;
    fill.right = fill.left + static_cast<int>(
        static_cast<double>(fill.right - fill.left) * progress.fraction);
    if (fill.right > fill.left) {
      fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue);
    }
    RECT progress_note{
        action_card.left + 16,
        456,
        layout.cancel.left - 12,
        477};
    draw_text(
        dc,
        progress.speed_label + L" / 残り " + progress.remaining_label,
        progress_note,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else if (state.image_execution_ready) {
    RECT title{
        action_card.left + 16,
        393,
        action_card.right - 16,
        415};
    draw_text(
        dc,
        L"内容確認済み — 大文字の OK だけを入力してください",
        title,
        state.body_font,
        kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT token_label{
        action_card.left + 16,
        417,
        action_card.right - 16,
        435};
    draw_text(
        dc,
        L"確認語: OK",
        token_label,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT note{
        action_card.left + 16,
        451,
        action_card.right - 16,
        471};
    draw_text(
        dc,
        L"実行直前にも作成元・保存先・レイアウトを再確認します。",
        note,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else {
    RECT guidance{
        action_card.left + 16,
        396,
        action_card.right - 16,
        471};
    draw_text(
        dc,
        L"作成元と保存先を選び、「内容を確認」を押してください。\r\n"
        L"この段階ではコピー元・保存先を変更せず、予約ジョブも作成しません。",
        guidance,
        state.body_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
  }

  if (client.bottom < 600) {
    return;
  }
  RECT result_card{
      content.left + 8,
      488,
      content.right - 8,
      content.bottom};
  fill_rounded_rect(dc, result_card, kCard, kBorder);
  RECT result_label{
      layout.output.left,
      491,
      layout.output.right,
      511};
  draw_text(
      dc,
      L"確認・作成結果",
      result_label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_image_restore_page(const AppState& state, HDC dc) {
  RECT client{};
  GetClientRect(state.window, &client);
  const RECT content = content_rect(state);
  const auto layout = ytec::winpeapp::build_winpe_image_restore_layout(
      client.right, client.bottom);
  draw_stepper(
      state,
      dc,
      {L"完全検証", L"復元先", L"最終確認", L"復元"},
      state.restore_step);

  RECT selection_card{content.left + 8, 168, content.right - 8, 388};
  fill_rounded_rect(dc, selection_card, kCard, kBorder);
  RECT label{
      layout.image_path.left,
      178,
      layout.image_path.right,
      198};
  draw_text(
      dc,
      L"復元イメージ（NTFS / exFAT上の単一 .tsumugi）",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  RECT verification_note{
      layout.image_path.left,
      245,
      layout.verify.left - 12,
      272};
  draw_text(
      dc,
      state.verified_restore_image.has_value()
          ? L"完全検証済み。復元先を選択できます。"
          : L"対象選択より先に、全チャンクと全体SHA-256を検証します。",
      verification_note,
      state.small_font,
      state.verified_restore_image.has_value() ? kSafeGreen : kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  label = RECT{
      layout.source_partition.left,
      274,
      layout.source_partition.right,
      294};
  draw_text(
      dc,
      state.verified_restore_image.has_value() &&
              state.verified_restore_image->mode ==
                  ytec::imageformat::TsumugiManifestMode::shrink
          ? L"復元範囲（縮小はディスク全体のみ）"
          : L"復元範囲",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  label = RECT{
      layout.target_partition.left,
      274,
      layout.target_partition.right,
      294};
  draw_text(
      dc,
      state.verified_restore_image.has_value() &&
              state.verified_restore_image->mode ==
                  ytec::imageformat::TsumugiManifestMode::shrink
          ? L"個別復元先（縮小では使用しません）"
          : L"復元先（既存区画／未割当の新規区画）",
      label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  label = RECT{
      layout.target.left,
      330,
      layout.target.right,
      350};
  draw_text(
      dc,
      selected_restore_source_partition(state).has_value()
          ? L"復元先ディスク（区画表と他のパーティションは保持）"
          : L"復元先ディスク（既存パーティションを含む全内容を消去）",
      label,
      state.small_font,
      kWarning,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

  RECT action_card{content.left + 8, 394, content.right - 8, 466};
  fill_rounded_rect(dc, action_card, kCard, kBorder);
  if (state.restore_progress_active) {
    const auto progress = ytec::winpeapp::build_operation_progress_view(
        state.restore_progress, state.restore_elapsed);
    RECT stage{
        action_card.left + 16,
        398,
        layout.cancel.left - 12,
        420};
    draw_text(
        dc,
        progress.stage_label,
        stage,
        state.body_font,
        progress.cancellation_allowed ? kTsumugiBlue : kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT percentage{
        action_card.left + 16,
        424,
        layout.cancel.left - 12,
        443};
    draw_text(
        dc,
        L"検証済み " + progress.percentage_label + L" / " +
            progress.speed_label + L" / 残り " + progress.remaining_label,
        percentage,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT track{
        action_card.left + 16,
        448,
        action_card.right - 16,
        458};
    fill_rounded_rect(dc, track, RGB(226, 233, 237), RGB(226, 233, 237));
    RECT fill = track;
    fill.right = fill.left + static_cast<int>(
        static_cast<double>(fill.right - fill.left) * progress.fraction);
    if (fill.right > fill.left) {
      fill_rounded_rect(dc, fill, kTsumugiBlue, kTsumugiBlue);
    }
  } else if (state.restore_execution_ready) {
    RECT title{
        action_card.left + 16,
        397,
        action_card.right - 16,
        419};
    draw_text(
        dc,
        L"消去対象を確認済み — 大文字の OK だけを入力してください",
        title,
        state.body_font,
        kWarning,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT note{
        action_card.left + 16,
        447,
        action_card.right - 16,
        462};
    draw_text(
        dc,
        L"実行直前にもイメージ、保存媒体、対象識別、レイアウトを再確認します。",
        note,
        state.small_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  } else {
    RECT guidance{
        action_card.left + 16,
        399,
        action_card.right - 16,
        460};
    draw_text(
        dc,
        state.verified_restore_image.has_value()
            ? L"復元範囲と復元先を選び、「上書き内容を確認」を押してください。\r\n"
              L"この段階ではどのディスクも変更しません。"
            : L".tsumugi を選び、「完全検証」を押してください。\r\n"
              L"旧 .dcimg/.dcmig と予約ジョブは使用しません。",
        guidance,
        state.body_font,
        kMuted,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
  }

  if (client.bottom < 600) {
    return;
  }
  RECT result_card{
      content.left + 8,
      476,
      content.right - 8,
      content.bottom};
  fill_rounded_rect(dc, result_card, kCard, kBorder);
  RECT result_label{
      layout.output.left,
      479,
      layout.output.right,
      501};
  draw_text(
      dc,
      L"検証・復元結果",
      result_label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_repair_page(const AppState& state, HDC dc) {
  const RECT content = content_rect(state);
  RECT client{};
  GetClientRect(state.window, &client);
  const bool compact_height = client.bottom < 600;
  draw_stepper(
      state,
      dc,
      {L"対象選択", L"自動解析", L"最終確認", L"修復"},
      state.repair_step);
  RECT target_card{
      content.left + 8,
      168,
      content.right - 8,
      compact_height ? 270 : 282};
  fill_rounded_rect(dc, target_card, kCard, kBorder);
  RECT target_label{
      target_card.left + 14,
      178,
      target_card.right - 14,
      198};
  draw_text(
      dc,
      L"対象ディスク",
      target_label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT target_note{
      target_card.left + 14,
      244,
      target_card.right - 14,
      compact_height ? 264 : 274};
  draw_text(
      dc,
      L"Windows・ESP／Active・MBR／GPT・BIOS／UEFIを読取専用で解析します。",
      target_note,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

  RECT action_card{
      content.left + 8,
      compact_height ? 280 : 294,
      content.right - 8,
      compact_height ? 390 : 424};
  fill_rounded_rect(dc, action_card, kCard, kBorder);
  RECT action_title{
      action_card.left + 16,
      action_card.top + 12,
      action_card.right - 16,
      action_card.top + 36};
  const wchar_t* action_text = L"自動解析結果を確認してください";
  COLORREF action_color = kMuted;
  if (state.boot_execution_active) {
    action_text =
        L"安全トランザクションを実行中（途中取消不可）";
    action_color = kWarning;
  } else if (state.inspected_boot_execution.has_value()) {
    action_text = L"確認済み — 大文字の OK だけで最終確定";
    action_color = kWarning;
  } else if (state.operation_busy && state.repair_step == 2) {
    action_text = L"読み取り専用で自動解析中です";
    action_color = kTsumugiBlue;
  }
  draw_text(
      dc,
      action_text,
      action_title,
      state.body_font,
      action_color,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT action_note{
      action_card.left + 16,
      action_card.top + 42,
      action_card.right - 16,
      compact_height ? action_card.top + 70 : action_card.bottom - 8};
  draw_text(
      dc,
      state.boot_execution_active
          ? L"実行直前に全レイアウトを再照合し、BCDBoot /cとBCDの通常ファイル存在確認を行います。"
          : state.inspected_boot_execution.has_value()
          ? L"表示した優先順のWindowsとシステム領域だけを対象にします。既存BCDは退避し、失敗時は一括ロールバックします。"
          : L"対象を選び「自動解析」を押してください。解析の段階ではBCDやディスクを変更しません。",
      action_note,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_WORDBREAK);

  RECT result_card{
      content.left + 8,
      compact_height ? 400 : 434,
      content.right - 8,
      content.bottom};
  fill_rounded_rect(dc, result_card, kCard, kBorder);
  if (compact_height) {
    return;
  }
  RECT result_label{
      result_card.left + 16,
      442,
      result_card.right - 16,
      458};
  draw_text(
      dc,
      L"自動解析・実行結果",
      result_label,
      state.small_font,
      kMuted,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_disk_page(const AppState& state, HDC dc) {
  paint_status_banner(state, dc);
  const RECT content = content_rect(state);
  RECT list_card{content.left + 8, 154, content.left + (content.right - content.left) * 40 / 100, content.bottom};
  RECT detail_card{list_card.right + 12, 154, content.right - 8, content.bottom};
  fill_rounded_rect(dc, list_card, kCard, kBorder);
  fill_rounded_rect(dc, detail_card, kCard, kBorder);
  RECT list_label{list_card.left + 16, 164, list_card.right - 16, 190};
  draw_text(
      dc,
      L"物理ディスク",
      list_label,
      state.body_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
  RECT detail_label{detail_card.left + 16, 164, detail_card.right - 16, 190};
  draw_text(
      dc,
      L"読み取り専用の詳細",
      detail_label,
      state.body_font,
      kInk,
      DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void paint_window(const AppState& state, HDC dc) {
  RECT client{};
  GetClientRect(state.window, &client);
  FillRect(dc, &client, state.canvas_brush);
  paint_sidebar(state, dc, client);
  paint_header(state, dc, client);
  if (state.page == Page::clone) {
    paint_clone_page(state, dc);
  } else if (state.page == Page::image_create) {
    paint_image_create_page(state, dc);
  } else if (state.page == Page::image_restore) {
    paint_image_restore_page(state, dc);
  } else if (state.page == Page::boot_repair) {
    paint_repair_page(state, dc);
  } else {
    paint_disk_page(state, dc);
  }
}

void draw_navigation_button(
    const AppState& state,
    const DRAWITEMSTRUCT& item) {
  Page page = Page::clone;
  if (item.CtlID == kNavImageCreateId) {
    page = Page::image_create;
  } else if (item.CtlID == kNavImageRestoreId) {
    page = Page::image_restore;
  } else if (item.CtlID == kNavRepairId) {
    page = Page::boot_repair;
  } else if (item.CtlID == kNavDiskId) {
    page = Page::disk_diagnostics;
  }
  const bool selected = state.page == page;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const COLORREF background =
      pressed ? RGB(64, 78, 96)
              : selected ? kSidebarSelected : kSidebar;
  const HBRUSH brush = CreateSolidBrush(background);
  FillRect(item.hDC, &item.rcItem, brush);
  DeleteObject(brush);

  RECT text_bounds = item.rcItem;
  text_bounds.left += 16;
  const wchar_t* text = L"ドライブをクローン";
  if (page == Page::image_create) {
    text = L"イメージを作成";
  } else if (page == Page::image_restore) {
    text = L"イメージを復元";
  } else if (page == Page::boot_repair) {
    text = L"起動を修復";
  } else if (page == Page::disk_diagnostics) {
    text = L"診断";
  }
  draw_text(
      item.hDC,
      text,
      text_bounds,
      state.body_font,
      selected ? RGB(255, 255, 255) : RGB(205, 217, 225),
      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  if ((item.itemState & ODS_FOCUS) != 0) {
    RECT focus = item.rcItem;
    InflateRect(&focus, -3, -3);
    DrawFocusRect(item.hDC, &focus);
  }
}

void change_page(AppState& state, const Page page) {
  if (state.operation_busy) {
    return;
  }
  state.page = page;
  show_page_controls(state);
  update_action_state(state);
  InvalidateRect(state.window, nullptr, TRUE);
  const HWND focus = page == Page::clone
      ? state.clone_path
      : page == Page::image_create
      ? state.image_source
      : page == Page::image_restore
      ? state.restore_path
      : page == Page::boot_repair
      ? state.repair_disk
      : state.disk_list;
  SetFocus(focus);
}

void initialize_controls(AppState& state) {
  state.nav_clone = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavCloneId);
  state.nav_image_create = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavImageCreateId);
  state.nav_image_restore = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavImageRestoreId);
  state.nav_repair = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavRepairId);
  state.nav_disk = create_control(
      state, L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kNavDiskId);
  state.refresh = create_control(
      state,
      L"BUTTON",
      L"再読込み",
      WS_TABSTOP | BS_PUSHBUTTON,
      kRefreshId);
  state.clone_path = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kClonePathId);
  state.clone_browse = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kCloneBrowseId);
  state.clone_check = create_control(
      state,
      L"BUTTON",
      L"読取り確認",
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      kCloneCheckId);
  state.clone_rescue_mode = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kCloneRescueModeId);
  state.clone_acknowledge = create_control(
      state,
      L"BUTTON",
      L"確認した対象ディスクの全パーティションとデータが消去されることを理解しました",
      WS_TABSTOP | BS_AUTOCHECKBOX,
      kCloneAcknowledgeId);
  state.clone_token = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kCloneTokenId);
  state.clone_execute = create_control(
      state,
      L"BUTTON",
      L"処理を開始",
      WS_TABSTOP | BS_PUSHBUTTON,
      kCloneExecuteId);
  state.clone_cancel = create_control(
      state,
      L"BUTTON",
      L"安全に取消",
      WS_TABSTOP | BS_PUSHBUTTON,
      kCloneCancelId);
  state.clone_pause = create_control(
      state,
      L"BUTTON",
      L"一時停止不可",
      WS_TABSTOP | BS_PUSHBUTTON,
      kClonePauseId);
  state.clone_output = create_control(
      state,
      L"EDIT",
      L"コピー元とコピー先を選択してください。\r\n"
      L"方式は「通常」「救出」「MBR→GPT（別ディスク）」「縮小移行」から明示選択します。\r\n"
      L"救出・形式変換・縮小移行は互いに同時実行できません。\r\n"
      L"読取り専用確認の後、このPE内で直接クローンを実行できます。",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
      ES_READONLY,
      kCloneOutputId);
  state.image_source = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kImageSourceId);
  state.image_destination = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kImageDestinationId);
  state.image_browse = create_control(
      state,
      L"BUTTON",
      L"保存先を選択",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageBrowseId);
  state.image_rescue_mode = create_control(
      state,
      L"BUTTON",
      L"救出モード（別ディスクの一時領域を経由し、欠損はゼロ埋め記録）",
      WS_TABSTOP | BS_AUTOCHECKBOX,
      kImageRescueModeId);
  state.image_verification_mode = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kImageVerificationModeId);
  state.image_encryption = create_control(
      state,
      L"BUTTON",
      L"暗号化（回復キーなし）",
      WS_TABSTOP | BS_AUTOCHECKBOX,
      kImageEncryptionId);
  state.image_review = create_control(
      state,
      L"BUTTON",
      L"内容を確認",
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      kImageReviewId);
  state.image_token = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kImageTokenId);
  state.image_execute = create_control(
      state,
      L"BUTTON",
      L"作成を開始",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageExecuteId);
  state.image_cancel = create_control(
      state,
      L"BUTTON",
      L"安全に取消",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageCancelId);
  state.image_pause = create_control(
      state,
      L"BUTTON",
      L"一時停止不可",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImagePauseId);
  state.image_output = create_control(
      state,
      L"EDIT",
      L"作成元と .tsumugi 保存先を選択してください。\r\n"
      L"完全検証（推奨）または高速検証を選択し、\r\n"
      L"故障が疑われる場合は救出モード、必要なら暗号化を選び、\r\n"
      L"予約ジョブを作らずこのPE内で直接作成します。",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kImageOutputId);
  state.restore_path = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kImageRestorePathId);
  state.restore_browse = create_control(
      state,
      L"BUTTON",
      L"イメージを選択",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageRestoreBrowseId);
  state.restore_verify = create_control(
      state,
      L"BUTTON",
      L"完全検証",
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      kImageRestoreVerifyId);
  state.restore_source_partition = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kImageRestoreSourcePartitionId);
  state.restore_target = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kImageRestoreTargetId);
  state.restore_target_partition = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kImageRestoreTargetPartitionId);
  state.restore_review = create_control(
      state,
      L"BUTTON",
      L"上書き内容を確認",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageRestoreReviewId);
  state.restore_token = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kImageRestoreTokenId);
  state.restore_execute = create_control(
      state,
      L"BUTTON",
      L"復元を開始",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageRestoreExecuteId);
  state.restore_cancel = create_control(
      state,
      L"BUTTON",
      L"安全に取消",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageRestoreCancelId);
  state.restore_pause = create_control(
      state,
      L"BUTTON",
      L"一時停止不可",
      WS_TABSTOP | BS_PUSHBUTTON,
      kImageRestorePauseId);
  state.restore_output = create_control(
      state,
      L"EDIT",
      L"既存の単一 .tsumugi ファイルを選択してください。\r\n"
      L"完全検証の後、このPE内で復元先を直接選択します。",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kImageRestoreOutputId);
  state.disk_list = create_control(
      state,
      L"LISTBOX",
      L"",
      WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY |
          LBS_NOINTEGRALHEIGHT,
      kDiskListId);
  state.disk_details = create_control(
      state,
      L"EDIT",
      L"",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kDiskDetailsId);
  state.repair_disk = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kRepairDiskId);
  state.windows_root = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
      kWindowsRootId);
  state.system_root = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kSystemRootId);
  state.firmware = create_control(
      state,
      L"COMBOBOX",
      L"",
      WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
      kFirmwareId);
  state.repair_inspect = create_control(
      state,
      L"BUTTON",
      L"自動解析",
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      kRepairInspectId);
  state.winre_diagnostic = create_control(
      state,
      L"BUTTON",
      L"WinREを診断",
      WS_TABSTOP | BS_PUSHBUTTON,
      kWinReDiagnosticId);
  state.repair_acknowledge = create_control(
      state,
      L"BUTTON",
      L"選択したWindowsの起動ファイルが変更されることを理解しました",
      WS_TABSTOP | BS_AUTOCHECKBOX,
      kRepairAcknowledgeId);
  state.repair_token = create_control(
      state,
      L"EDIT",
      L"",
      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
      kRepairTokenId);
  state.repair_cancel = create_control(
      state,
      L"BUTTON",
      L"確認を取消",
      WS_TABSTOP | BS_PUSHBUTTON,
      kRepairCancelId);
  state.repair_execute = create_control(
      state,
      L"BUTTON",
      L"起動修復を実行",
      WS_TABSTOP | BS_PUSHBUTTON,
      kRepairExecuteId);
  state.repair_output = create_control(
      state,
      L"EDIT",
      L"対象ディスクを一つ選び、「自動解析」を押してください。\r\n"
      L"Windows、システム領域、MBR/GPT、起動方式を読み取り専用で確認します。",
      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_READONLY,
      kRepairOutputId);

  state.title_font = CreateFontW(
      -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.bold_face());
  state.heading_font = CreateFontW(
      -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.bold_face());
  state.body_font = CreateFontW(
      -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.regular_face());
  state.small_font = CreateFontW(
      -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, state.private_fonts.regular_face());
  state.mono_font = CreateFontW(
      -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  state.canvas_brush = CreateSolidBrush(kCanvas);
  state.card_brush = CreateSolidBrush(kCard);

  for (const HWND control :
       {state.nav_clone,
        state.nav_image_create,
        state.nav_image_restore,
        state.nav_repair,
        state.nav_disk,
        state.refresh,
         state.clone_path,
         state.clone_browse,
         state.clone_check,
         state.clone_rescue_mode,
         state.clone_acknowledge,
         state.clone_token,
         state.clone_execute,
         state.clone_cancel,
         state.clone_pause,
        state.image_source,
        state.image_destination,
        state.image_browse,
        state.image_rescue_mode,
        state.image_verification_mode,
        state.image_encryption,
        state.image_review,
        state.image_token,
         state.image_execute,
         state.image_cancel,
         state.image_pause,
        state.restore_path,
        state.restore_browse,
        state.restore_verify,
        state.restore_source_partition,
        state.restore_target,
        state.restore_target_partition,
        state.restore_review,
        state.restore_token,
         state.restore_execute,
         state.restore_cancel,
         state.restore_pause,
        state.disk_list,
        state.repair_disk,
        state.windows_root,
        state.system_root,
        state.firmware,
        state.winre_diagnostic,
        state.repair_inspect,
        state.repair_acknowledge,
        state.repair_token,
        state.repair_execute,
        state.repair_cancel}) {
    set_control_font(control, state.body_font);
  }
  set_control_font(state.clone_output, state.small_font);
  set_control_font(state.image_output, state.small_font);
  set_control_font(state.restore_output, state.small_font);
  set_control_font(state.disk_details, state.small_font);
  set_control_font(state.repair_output, state.small_font);
  SendMessageW(
      state.clone_rescue_mode,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"通常モード（完全複製）"));
  SendMessageW(
      state.clone_rescue_mode,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(
          L"救出モード（未読範囲はゼロ埋め／形式変換なし）"));
  SendMessageW(
      state.clone_rescue_mode,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"MBR→GPT（別ディスク）"));
  SendMessageW(
      state.clone_rescue_mode,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(
          L"縮小移行（WinPE / GPT / 512B）"));
  SendMessageW(state.clone_rescue_mode, CB_SETCURSEL, 0, 0);
  SendMessageW(
      state.image_verification_mode,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"検証: 完全（推奨）"));
  SendMessageW(
      state.image_verification_mode,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(
          L"検証: 高速（完成後の追加全走査のみ省略）"));
  SendMessageW(state.image_verification_mode, CB_SETCURSEL, 0, 0);
  SendMessageW(
      state.firmware,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"UEFI（GPT）"));
  SendMessageW(
      state.firmware,
      CB_ADDSTRING,
      0,
      reinterpret_cast<LPARAM>(L"BIOS（MBR）"));
  SendMessageW(state.firmware, CB_SETCURSEL, 0, 0);
  populate_root_controls(state);
  show_page_controls(state);
  layout_controls(state);
}

[[nodiscard]] bool inspect_image_create_resume_on_startup(AppState& state) {
  auto inspected = inspect_product_image_create_resume();
  if (!inspected) {
    state.image_resume_platform_ready = false;
    state.image_resume_slot_present = true;
    state.restore_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"単一再開slotを通常イメージ作成用として安全に分類できません。\r\n"
        L"unknown、corrupt、orphan、relink、またはidentity不一致は自動修復／削除せず保持し、"
        L"全ての新しい書込み操作を開始しません。",
        L"イメージ作成の再開情報を確認できません",
        MB_OK | MB_ICONWARNING);
    return false;
  }
  state.image_resume_platform_ready = true;
  if (inspected.value().object_state == ytec::operationcore::
          PersistentPeExactImageCreateObjectState::no_slot ||
      inspected.value().object_state == ytec::operationcore::
          PersistentPeExactImageCreateObjectState::other_capability) {
    state.image_resume_slot_present = false;
    state.image_resume_requested = false;
    state.reviewed_image_resume_observation.reset();
    return true;
  }

  auto summary = ytec::winpeapp::
      format_direct_image_create_resume_startup_review_v1(
          inspected.value());
  if (!summary || !inspected.value().binding ||
      !inspected.value().continuity || !inspected.value().source) {
    state.image_resume_platform_ready = false;
    state.image_resume_slot_present = true;
    state.restore_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"persistent exactイメージ作成の安全な要約または完全bindingを作れません。\r\n"
        L"再開情報と所有ファイルは削除せず保持し、全ての新しい書込み操作を開始しません。",
        L"イメージ作成の再開情報を表示できません",
        MB_OK | MB_ICONWARNING);
    return false;
  }

  state.image_resume_slot_present = true;
  state.reviewed_image_resume_observation = inspected.value();
  // The same fixed slot has been authenticated.  Restore routing must not try
  // to reinterpret capability 8, while every new route remains blocked by
  // the global fresh observation gate.
  state.restore_resume_platform_ready = true;
  const int decision = MessageBoxW(
      state.window,
      summary.value().c_str(),
      L"前回中断した通常イメージ作成があります",
      MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
  if (decision == IDYES) {
    state.image_resume_requested = true;
    state.page = Page::image_create;
    state.image_step = 1;
    SetWindowTextW(
        state.image_destination, inspected.value().final_path.c_str());
    SendMessageW(state.image_rescue_mode, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(
        state.image_encryption,
        BM_SETCHECK,
        inspected.value().continuity->encrypted
            ? BST_CHECKED
            : BST_UNCHECKED,
        0);
    SendMessageW(
        state.image_verification_mode,
        CB_SETCURSEL,
        inspected.value().continuity->verification_mode ==
                ytec::imageformat::TsumugiCreateVerificationMode::complete
            ? 0
            : 1,
        0);
    SetWindowTextW(
        state.image_output,
        L"再開準備を選びました。同じSourceを読み取り専用一覧から再選択し、"
        L"内容確認でsource状態・保存先・partial/journal File ID・計画を再証明します。\r\n"
        L"暗号化されている場合、保存していないパスワードを再入力してください。");
    show_page_controls(state);
    update_action_state(state);
    return false;
  }
  state.image_resume_requested = false;
  if (decision != IDNO) {
    return false;
  }
  if (inspected.value().object_state != ytec::operationcore::
          PersistentPeExactImageCreateObjectState::staged) {
    MessageBoxW(
        state.window,
        L"完成名へ公開済みのFile IDは破棄できません。\r\n"
        L"再開を選び、完成 .tsumugi の全量検証とjournal／checkpoint整理を完了してください。"
        L"既存の別ファイルを採用または上書きすることはありません。",
        L"完全検証と整理の再開が必要です",
        MB_OK | MB_ICONWARNING);
    return false;
  }
  const int discard_confirm = MessageBoxW(
      state.window,
      L"アプリが所有する、表示済みFile IDと一致する .tsumugi.partial、"
      L"private journal、active.checkpointだけをfresh exact-open後に破棄します。\r\n"
      L"完成 .tsumugi や別ファイルは削除しません。\r\n\r\n"
      L"再開情報を破棄しますか？",
      L"イメージ作成の再開情報を最終確認",
      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (discard_confirm != IDYES) {
    return false;
  }
  auto storage = ytec::winpeapp::
      make_direct_image_create_windows_resume_storage_platform_v1();
  auto platform = storage
      ? ytec::operationcore::
            make_current_executable_windows_resume_slot_platform(
                storage.value().prove_data_backing)
      : ytec::clonecore::Result<std::unique_ptr<
            ytec::operationcore::IResumeSlotPlatform>>::failure(
            storage.error());
  auto discarded = platform
      ? ytec::winpeapp::discard_direct_image_create_resume_v1(
            *inspected.value().binding, *platform.value())
      : ytec::clonecore::Status::failure(platform.error());
  if (!discarded) {
    state.image_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"表示後にbindingまたはFile IDが変化したため、何も削除せず保持しました。\r\n"
        L"全ての新しい書込み操作を開始しません。",
        L"再開情報を破棄できません",
        MB_OK | MB_ICONWARNING);
    return false;
  }
  state.image_resume_slot_present = false;
  state.reviewed_image_resume_observation.reset();
  MessageBoxW(
      state.window,
      L"exact bindingが一致したアプリ所有のpartial、journal、checkpointだけを破棄しました。\r\n"
      L"完成 .tsumugi、Source、その他のファイルは変更していません。",
      L"イメージ作成の再開情報を破棄しました",
      MB_OK | MB_ICONINFORMATION);
  return false;
}

void inspect_restore_resume_on_startup(AppState& state) {
#ifdef YTEC_UI_ACCEPTANCE_BUILD
  // The host-side acceptance executable has no product media marker and must
  // never inspect the host's portable data. Product wiring remains compiled
  // unchanged into ytec-winpe-gui.
  state.image_resume_platform_ready = true;
  state.restore_resume_platform_ready = true;
  return;
#else
  if (!inspect_image_create_resume_on_startup(state)) {
    return;
  }
  auto storage = ytec::winpeapp::
      make_direct_image_restore_windows_storage_platform_v1();
  if (!storage) {
    state.restore_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"単一再開slotの保存先を安全に確認できません。\r\n"
        L"slotが空であることを証明できないため、クローン、イメージ作成／復元、"
        L"媒体作成、起動修復を含む全ての新規書込み操作を開始しません。",
        L"復元の再開情報を確認できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  auto slot = ytec::operationcore::
      make_current_executable_windows_resume_slot_platform(
          storage.value().prove_data_backing);
  if (!slot) {
    state.restore_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"実行ファイル隣の data を永続再開用として確認できません。\r\n"
        L"slotが空であることを証明できないため、全ての新規書込み操作を開始しません。",
        L"復元の再開情報を確認できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  const ytec::winpeapp::DirectImageRestoreRequest empty_request{};
  const ytec::winpeapp::DirectImageRestoreResumeDependencies inspect_deps{
      .slot_platform = slot.value().get(),
  };
  auto inspected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      empty_request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::inspect_only,
      },
      inspect_deps);
  if (!inspected) {
    state.restore_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"再開情報が不明または破損しているため、自動削除せず保持しました。\r\n"
        L"orphan、identity不一致を含め安全に空と証明できない状態では、"
        L"全ての新規書込み操作を開始しません。",
        L"復元の再開情報を確認できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  state.restore_resume_platform_ready = true;
  if (inspected.value().kind ==
      ytec::winpeapp::DirectImageRestoreResumeOutcomeKind::no_slot) {
    state.restore_resume_slot_present = false;
    return;
  }
  auto summary = ytec::winpeapp::
      format_direct_image_restore_resume_startup_review_v1(
          inspected.value());
  if (!summary || !inspected.value().existing_slot) {
    state.restore_resume_platform_ready = false;
    state.restore_resume_slot_present = true;
    MessageBoxW(
        state.window,
        L"再開情報の安全な要約を作成できないため、自動削除せず保持しました。\r\n"
        L"永続exact／rescue復元として安全に表示できないslotは再開せず、"
        L"全ての新規書込み操作も開始しません。",
        L"復元の再開情報を確認できません",
        MB_OK | MB_ICONWARNING);
    return;
  }

  state.restore_resume_slot_present = true;
  state.reviewed_restore_resume_slot = inspected.value().existing_slot;
  const int decision = MessageBoxW(
      state.window,
      summary.value().c_str(),
      L"前回中断した復元があります",
      MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
  if (decision == IDYES) {
    state.restore_resume_requested = true;
    state.page = Page::image_restore;
    state.restore_step = 1;
    SetWindowTextW(
        state.restore_output,
        L"再開準備を選びました。前回と同じ .tsumugi を完全検証し、"
        L"同じ復元先を選び直してください。\r\n"
        L"opened-handle 4者証明と復元計画の完全一致後だけ再開します。");
    show_page_controls(state);
    update_action_state(state);
    return;
  }
  if (decision != IDNO) {
    state.restore_resume_requested = false;
    return;
  }
  const int discard_confirm = MessageBoxW(
      state.window,
      L"この操作はアプリが所有する active.checkpoint と、"
      L"そこに結び付いた partial だけを破棄します。\r\n"
      L"復元先ディスクや .tsumugi イメージは変更しません。\r\n\r\n"
      L"再開情報を破棄しますか？",
      L"再開情報の破棄を最終確認",
      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
  if (discard_confirm != IDYES) {
    state.restore_resume_requested = false;
    return;
  }
  auto discarded = ytec::winpeapp::control_direct_image_restore_resume_v1(
      empty_request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::discard_existing,
          .reviewed_existing_slot = inspected.value().existing_slot,
      },
      inspect_deps);
  if (!discarded || discarded.value().kind !=
          ytec::winpeapp::DirectImageRestoreResumeOutcomeKind::discarded) {
    state.restore_resume_platform_ready = false;
    MessageBoxW(
        state.window,
        L"表示後に再開情報が変化したか、安全な所有確認ができませんでした。\r\n"
        L"何も削除せず保持しました。",
        L"再開情報を破棄できません",
        MB_OK | MB_ICONWARNING);
    return;
  }
  state.restore_resume_slot_present = false;
  state.reviewed_restore_resume_slot.reset();
  state.restore_resume_requested = false;
  MessageBoxW(
      state.window,
      L"アプリが所有する再開情報を破棄しました。\r\n"
      L"復元先ディスクと .tsumugi イメージは変更していません。",
      L"再開情報を破棄しました",
      MB_OK | MB_ICONINFORMATION);
#endif
}

void destroy_state_resources(AppState& state) {
  state.reviewed_image_password.reset();
  state.reviewed_restore_image_password.reset();
  for (const HFONT font :
       {state.title_font,
        state.heading_font,
        state.body_font,
        state.small_font,
        state.mono_font}) {
    if (font != nullptr) {
      DeleteObject(font);
    }
  }
  if (state.canvas_brush != nullptr) {
    DeleteObject(state.canvas_brush);
  }
  if (state.card_brush != nullptr) {
    DeleteObject(state.card_brush);
  }
}

LRESULT CALLBACK window_proc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));

  if (message == WM_CREATE) {
    auto created = std::make_unique<AppState>();
    created->window = window;
    created->dashboard =
        ytec::winpeapp::DashboardView{
            .readiness = ytec::winpeapp::DashboardReadiness::scanning,
            .headline = L"起動環境を準備しています",
            .guidance = L"読み取り専用のディスク確認を開始します。"};
    state = created.release();
    SetWindowLongPtrW(
        window,
        GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(state));
    static_cast<void>(state->private_fonts.load_line_seed_jp(
        reinterpret_cast<HMODULE>(
            GetWindowLongPtrW(window, GWLP_HINSTANCE))));
    initialize_controls(*state);
    inspect_restore_resume_on_startup(*state);
    start_inventory(*state);
    return 0;
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }

  switch (message) {
    case WM_SIZE:
      layout_controls(*state);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
      // At 200% GDI scaling, keep the minimum inside a 1920 x 1080
      // work area. Compact layouts omit only the secondary result panes.
      limits->ptMinTrackSize.x = 960;
      limits->ptMinTrackSize.y = 516;
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      const HDC dc = BeginPaint(window, &paint);
      paint_window(*state, dc);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
      if (item != nullptr &&
          (item->CtlID == kNavCloneId ||
           item->CtlID == kNavImageCreateId ||
           item->CtlID == kNavImageRestoreId ||
           item->CtlID == kNavRepairId ||
           item->CtlID == kNavDiskId)) {
        draw_navigation_button(*state, *item);
        return TRUE;
      }
      break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      const HDC control_dc = reinterpret_cast<HDC>(wparam);
      SetBkColor(control_dc, kCard);
      SetTextColor(control_dc, kInk);
      return reinterpret_cast<LRESULT>(state->card_brush);
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int notification = HIWORD(wparam);
      if (id == kNavCloneId && notification == BN_CLICKED) {
        change_page(*state, Page::clone);
      } else if (id == kNavImageCreateId && notification == BN_CLICKED) {
        change_page(*state, Page::image_create);
      } else if (id == kNavImageRestoreId && notification == BN_CLICKED) {
        change_page(*state, Page::image_restore);
      } else if (id == kNavRepairId && notification == BN_CLICKED) {
        change_page(*state, Page::boot_repair);
      } else if (id == kNavDiskId && notification == BN_CLICKED) {
        change_page(*state, Page::disk_diagnostics);
      } else if (id == kRefreshId && notification == BN_CLICKED) {
        start_inventory(*state);
      } else if (id == kCloneCheckId && notification == BN_CLICKED) {
        start_clone_check(*state);
      } else if (id == kCloneExecuteId && notification == BN_CLICKED) {
        start_clone_execute(*state);
      } else if (id == kCloneCancelId && notification == BN_CLICKED) {
        request_clone_cancellation(*state);
      } else if (id == kClonePauseId && notification == BN_CLICKED) {
        handle_manual_pause_button(
            *state,
            state->clone_pause_controller,
            state->clone_output);
      } else if (id == kCloneRescueModeId &&
                 notification == CBN_SELCHANGE) {
        if (!state->operation_busy) {
          state->clone_step = 1;
          const CloneRoute route = selected_clone_route(*state);
          invalidate_clone_review(
              *state,
              route == CloneRoute::rescue
                  ? L"救出モードを選択しました。コピー元・コピー先を確認し、"
                    L"「読取り確認」を行ってください。"
                  : route == CloneRoute::mbr_to_gpt
                  ? L"MBR→GPT（別ディスク）を選択しました。"
                    L"コピー元MBRは変更せず、コピー先だけを消去・変換します。"
                    L"「読取り確認」を行ってください。"
                  : route == CloneRoute::shrink
                  ? L"縮小移行を選択しました。正式なWinPE上でコピー元をread-onlyに固定し、"
                    L"自動解析後にパーティションと余剰容量配分を選択します。"
                    L"「読取り確認」を行ってください。"
                  : L"通常モードを選択しました。読取りエラー時は停止します。"
                    L"「読取り確認」をもう一度行ってください。");
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageBrowseId && notification == BN_CLICKED) {
        choose_image_destination(*state);
      } else if (id == kImageReviewId && notification == BN_CLICKED) {
        start_image_review(*state);
      } else if (id == kImageExecuteId && notification == BN_CLICKED) {
        start_image_execute(*state);
      } else if (id == kImageCancelId && notification == BN_CLICKED) {
        request_image_cancellation(*state);
      } else if (id == kImagePauseId && notification == BN_CLICKED) {
        handle_manual_pause_button(
            *state,
            state->image_pause_controller,
            state->image_output);
      } else if (id == kImageRestoreBrowseId &&
                 notification == BN_CLICKED) {
        choose_restore_image(*state);
      } else if (id == kImageRestoreVerifyId &&
                 notification == BN_CLICKED) {
        start_restore_image_verification(*state);
      } else if (id == kImageRestoreReviewId &&
                 notification == BN_CLICKED) {
        start_restore_target_review(*state);
      } else if (id == kImageRestoreExecuteId &&
                 notification == BN_CLICKED) {
        start_restore_execute(*state);
      } else if (id == kImageRestoreCancelId &&
                  notification == BN_CLICKED) {
        request_restore_cancellation(*state);
      } else if (id == kImageRestorePauseId &&
                 notification == BN_CLICKED) {
        handle_manual_pause_button(
            *state,
            state->restore_pause_controller,
            state->restore_output);
      } else if ((id == kClonePathId || id == kCloneBrowseId) &&
                 notification == CBN_SELCHANGE) {
        state->clone_step = 1;
        if (!state->operation_busy) {
          invalidate_clone_review(
              *state,
              L"クローン対象が変更されました。"
              L"「読取り確認」をもう一度行ってください。");
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageRestorePathId &&
                 notification == EN_CHANGE) {
        if (!state->operation_busy) {
          invalidate_restore_image_review(
              *state,
              L"復元イメージが変更されました。"
              L"「完全検証」をもう一度行ってください。");
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageRestoreSourcePartitionId &&
                 notification == CBN_SELCHANGE) {
        if (!state->operation_busy) {
          invalidate_restore_target_review(
              *state,
              L"復元範囲が変更されました。"
              L"「上書き内容を確認」をもう一度行ってください。");
          populate_restore_target_partition_candidates(*state);
          show_page_controls(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageRestoreTargetId &&
                 notification == CBN_SELCHANGE) {
        if (!state->operation_busy) {
          invalidate_restore_target_review(
              *state,
              L"復元先が変更されました。"
              L"「上書き内容を確認」をもう一度行ってください。");
          populate_restore_target_partition_candidates(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageRestoreTargetPartitionId &&
                 notification == CBN_SELCHANGE) {
        if (!state->operation_busy) {
          invalidate_restore_target_review(
              *state,
              L"復元先パーティションが変更されました。"
              L"「上書き内容を確認」をもう一度行ってください。");
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if ((id == kImageSourceId &&
                  notification == CBN_SELCHANGE) ||
                 (id == kImageDestinationId &&
                  notification == EN_CHANGE)) {
        if (!state->operation_busy) {
          invalidate_image_review(
              *state,
              L"作成元または保存先が変更されました。"
              L"「内容を確認」をもう一度行ってください。");
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageVerificationModeId &&
                 notification == CBN_SELCHANGE) {
        if (!state->operation_busy) {
          const auto mode =
              selected_image_create_verification_mode(*state);
          invalidate_image_review(
              *state,
              mode == ytec::imageformat::
                          TsumugiCreateVerificationMode::fast
                  ? L"高速検証を選択しました。各書込み読戻し、認証・Hash、"
                    L"最終メタデータ検証を維持し、完成前の追加全走査だけを省略します。"
                  : mode == ytec::imageformat::
                                TsumugiCreateVerificationMode::complete
                  ? L"完全検証を選択しました。完成前の追加全走査も実行します。"
                  : L"検証方式を確認できません。選び直してください。"
          );
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageEncryptionId &&
                 notification == BN_CLICKED) {
        if (!state->operation_busy) {
          invalidate_image_review(
              *state,
              L"暗号化の選択が変更されました。"
              L"「内容を確認」をもう一度行ってください。");
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kImageRescueModeId &&
                 notification == BN_CLICKED) {
        if (!state->operation_busy) {
          invalidate_image_review(
              *state,
              SendMessageW(
                  state->image_rescue_mode, BM_GETCHECK, 0, 0) ==
                      BST_CHECKED
                  ? L"救出イメージ作成を選択しました。別ディスク上にRAW一時領域と画像の両方を保存できる空きが必要です。"
                    L"「内容を確認」をもう一度行ってください。"
                  : L"通常（exact）イメージ作成を選択しました。"
                    L"「内容を確認」をもう一度行ってください。");
          InvalidateRect(window, nullptr, FALSE);
        }
      } else if (id == kDiskListId && notification == LBN_SELCHANGE) {
        update_disk_details(*state);
      } else if (id == kRepairInspectId &&
                 notification == BN_CLICKED) {
        start_boot_inspect(*state);
      } else if (id == kWinReDiagnosticId &&
                 notification == BN_CLICKED) {
        start_winre_diagnostic(*state);
      } else if (id == kRepairExecuteId &&
                 notification == BN_CLICKED) {
        start_boot_execute(*state);
      } else if (id == kRepairCancelId &&
                 notification == BN_CLICKED) {
        cancel_boot_review(*state);
      } else if (id == kFirmwareId &&
                 notification == CBN_SELCHANGE) {
        select_default_roots(*state);
        invalidate_boot_review(*state);
      } else if ((id == kRepairDiskId &&
                  notification == CBN_SELCHANGE) ||
                 ((id == kWindowsRootId || id == kSystemRootId) &&
                  (notification == CBN_SELCHANGE ||
                   notification == CBN_EDITCHANGE))) {
        invalidate_boot_review(*state);
      }
      update_action_state(*state);
      return 0;
    }
    case kManualPauseStateChangedMessage:
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    case kInventoryCompleteMessage: {
      std::unique_ptr<InventoryPayload> payload(
          reinterpret_cast<InventoryPayload*>(lparam));
      state->inventory_busy = false;
      if (payload != nullptr && payload->inventory.has_value()) {
        state->inventory = std::move(payload->inventory);
        state->dashboard =
            ytec::winpeapp::build_dashboard_view(state->inventory.value());
        populate_disk_controls(*state);
        if (state->image_resume_requested &&
            state->reviewed_image_resume_observation.has_value()) {
          const bool source_selected =
              state->reviewed_image_resume_observation->source.has_value() &&
              selected_disk_number(state->image_source) ==
                  std::optional<std::uint32_t>(
                      state->reviewed_image_resume_observation->source
                          ->disk_number);
          SetWindowTextW(
              state->image_output,
              source_selected
                  ? L"前回と同じdisk number候補を選択しました。"
                    L"「内容を確認」でstable identity、source状態、保存先、"
                    L"partial/journal File ID、計画を再証明してください。"
                  : L"前回のSource候補を現在の一覧で選択できません。"
                    L"checkpointと所有ファイルは変更せず保持しました。");
        }
      } else {
        state->inventory.reset();
        state->dashboard =
            ytec::winpeapp::DashboardView{
                .readiness = ytec::winpeapp::DashboardReadiness::blocked,
                .headline = L"ディスク情報を取得できません",
                .guidance =
                    payload == nullptr || payload->error.empty()
                    ? L"再読込みしてください。処理は開始できません。"
                    : payload->error};
        SetWindowTextW(
            state->disk_details,
            state->dashboard.guidance.c_str());
      }
      update_action_state(*state);
      InvalidateRect(window, nullptr, TRUE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
      }
      return 0;
    }
    case kCloneShrinkInspectionCompleteMessage: {
      std::unique_ptr<CloneShrinkInspectionPayload> payload(
          reinterpret_cast<CloneShrinkInspectionPayload*>(lparam));
      state->operation_busy = false;
      if (payload == nullptr || !payload->inspection.has_value()) {
        invalidate_clone_review(
            *state,
            payload == nullptr || payload->output.empty()
                ? L"縮小移行の自動解析結果を取得できないため、"
                  L"コピー先を変更せず停止しました。"
                : payload->output);
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
        if (payload != nullptr) {
          show_captured_error(window, payload->error_presentation);
        }
        return 0;
      }

      SetWindowTextW(state->clone_output, payload->output.c_str());
      const auto choice = show_direct_shrink_partition_review_dialog(
          window, state->body_font, *payload->inspection);
      if (!choice.has_value()) {
        invalidate_clone_review(
            *state,
            L"パーティション選択と余剰容量設定を取り消しました。\r\n"
            L"コピー先は変更していません。"
            L"コピー元は安全のためOS-level read-onlyのままです。");
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }

      payload->request.selected_source_table_indexes =
          choice->selected_source_table_indexes;
      payload->request.surplus_allocation = choice->surplus_allocation;
      payload->request.surplus_target_source_table_index =
          choice->surplus_target_source_table_index;
      auto binding = payload->inspection->binding;
      start_clone_shrink_plan_after_review(
          *state, std::move(payload->request), std::move(binding));
      return 0;
    }
    case kCloneCheckCompleteMessage: {
      std::unique_ptr<CloneCheckPayload> payload(
          reinterpret_cast<CloneCheckPayload*>(lparam));
      state->operation_busy = false;
      state->clone_execution_ready =
          payload != nullptr && payload->execution_ready &&
          (payload->route == CloneRoute::rescue
               ? payload->reviewed_rescue_plan.has_value() &&
                    !payload->reviewed_plan.has_value() &&
                    !payload->reviewed_mbr2gpt_plan.has_value() &&
                    payload->reviewed_direct_shrink_plan == nullptr
               : payload->route == CloneRoute::mbr_to_gpt
               ? payload->reviewed_mbr2gpt_plan.has_value() &&
                    !payload->reviewed_plan.has_value() &&
                    !payload->reviewed_rescue_plan.has_value() &&
                    payload->reviewed_direct_shrink_plan == nullptr
               : payload->route == CloneRoute::shrink
               ? payload->reviewed_direct_shrink_plan != nullptr &&
                    !payload->reviewed_plan.has_value() &&
                    !payload->reviewed_rescue_plan.has_value() &&
                    !payload->reviewed_mbr2gpt_plan.has_value()
               : payload->reviewed_plan.has_value() &&
                    !payload->reviewed_rescue_plan.has_value() &&
                    !payload->reviewed_mbr2gpt_plan.has_value() &&
                    payload->reviewed_direct_shrink_plan == nullptr);
      state->reviewed_clone_plan =
          state->clone_execution_ready && payload->reviewed_plan.has_value()
          ? std::move(payload->reviewed_plan)
          : std::nullopt;
      state->reviewed_rescue_clone_plan =
          state->clone_execution_ready &&
              payload->reviewed_rescue_plan.has_value()
          ? std::move(payload->reviewed_rescue_plan)
          : std::nullopt;
      state->reviewed_mbr2gpt_clone_plan =
          state->clone_execution_ready &&
              payload->reviewed_mbr2gpt_plan.has_value()
          ? std::move(payload->reviewed_mbr2gpt_plan)
          : std::nullopt;
      state->reviewed_direct_shrink_clone_plan =
          state->clone_execution_ready
          ? std::move(payload->reviewed_direct_shrink_plan)
          : nullptr;
      state->clone_confirmation_token =
          state->clone_execution_ready ? L"OK" : L"";
      state->clone_step = state->clone_execution_ready ? 3 : 1;
      if (payload != nullptr) {
        std::wstring text = payload->output;
        if (state->clone_execution_ready) {
          text +=
              L"\r\n\r\n必須安全確認に合格しました。"
              L"ここまではコピー先を変更していません。"
              L"\r\n最終確認語: OK";
        }
        SetWindowTextW(state->clone_output, text.c_str());
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
      }
      return 0;
    }
    case kCloneProgressMessage: {
      std::unique_ptr<CloneProgressPayload> payload(
          reinterpret_cast<CloneProgressPayload*>(lparam));
      if (payload != nullptr && state->clone_progress_active) {
        state->clone_progress = payload->progress;
        state->clone_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kCloneExecuteCompleteMessage: {
      std::unique_ptr<CloneExecutePayload> payload(
          reinterpret_cast<CloneExecutePayload*>(lparam));
      state->operation_busy = false;
      state->clone_progress_active = false;
      state->clone_execution_ready = false;
      state->reviewed_clone_plan.reset();
      state->reviewed_rescue_clone_plan.reset();
      state->reviewed_mbr2gpt_clone_plan.reset();
      state->reviewed_direct_shrink_clone_plan.reset();
      state->clone_cancellation.reset();
      state->clone_pause_controller.reset();
      state->clone_confirmation_token.clear();
      SendMessageW(
          state->clone_acknowledge, BM_SETCHECK, BST_UNCHECKED, 0);
      SetWindowTextW(state->clone_token, L"");
      state->clone_step = payload != nullptr && payload->success ? 4 : 1;
      if (payload != nullptr) {
        std::wstring text = payload->output;
        if (payload->route == CloneRoute::rescue) {
          text += payload->success
              ? L"\r\n\r\n救出結果は通常クローン成功・起動成功扱いではありません。"
                L"実欠損mapを確認し、コピー先はofflineのまま別途検査してください。"
                L"コピー元はread-onlyのまま保持しています。"
              : L"\r\n\r\n救出完了扱いにしていません。"
                L"安全停止後もコピー元はread-only、コピー先はofflineのまま保護されている可能性があります。";
        } else if (payload->route == CloneRoute::mbr_to_gpt) {
          text += payload->success
              ? L"\r\n\r\n検証完了・換装待ちです。"
                L"コピー元MBRは変更していません。電源を切り、コピー元を外してUEFI起動を確認してください。"
              : L"\r\n\r\n完了扱いにしていません。"
                L"コピー先は可能な限りoffline、コピー元はread-onlyのまま保護しています。"
                L"対象を取り違えず診断してください。";
        } else if (payload->route == CloneRoute::shrink) {
          text += payload->success
              ? L"\r\n\r\n必須証拠検証完了・換装待ちです。"
                L"コピー先はoffline、コピー元はOS-level read-onlyのままです。"
                L"電源を切り、コピー元を外してから実機起動を確認してください。"
              : L"\r\n\r\n縮小移行を完了扱いにしていません。"
                L"コピー先は未変更、または書込み開始後ならabortしてofflineのまま、"
                L"コピー元はOS-level read-onlyのまま保護されます。";
        } else {
          text += payload->success
              ? L"\r\n\r\n検証完了・換装待ちです。"
                L"電源を切り、コピー元を外してから起動を確認してください。"
              : L"\r\n\r\n完了扱いにしていません。"
                L"コピー先がオフラインのまま保護されている可能性があります。";
        }
        SetWindowTextW(state->clone_output, text.c_str());
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
        const std::wstring_view operation_name =
            payload->route == CloneRoute::mbr_to_gpt
            ? L"MBR→GPT別ディスク移行"
            : payload->route == CloneRoute::shrink
            ? L"WinPE縮小移行クローン"
            : payload->route == CloneRoute::rescue
            ? L"救出クローン"
            : L"直接クローン";
        static_cast<void>(offer_winpe_completion_power_action(
            *state,
            payload->completion_power_proof,
            operation_name,
            payload->completion_power_target));
      }
      return 0;
    }
    case kImageCreateProgressMessage: {
      std::unique_ptr<ImageCreateProgressPayload> payload(
          reinterpret_cast<ImageCreateProgressPayload*>(lparam));
      if (payload != nullptr && state->image_progress_active) {
        state->image_progress = payload->progress;
        state->image_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kImageCreateCompleteMessage: {
      std::unique_ptr<ImageCreatePayload> payload(
          reinterpret_cast<ImageCreatePayload*>(lparam));
      state->operation_busy = false;
      state->image_progress_active = false;
      state->image_execution_ready = false;
      state->reviewed_image_source.reset();
      state->reviewed_image_partition_selection.reset();
      state->reviewed_image_path.clear();
      state->reviewed_image_replace_existing = false;
      state->reviewed_image_encrypted = false;
      state->reviewed_image_rescue_mode = false;
      state->reviewed_image_verification_mode =
          ytec::imageformat::TsumugiCreateVerificationMode::complete;
      state->reviewed_image_password.reset();
      if (payload != nullptr && payload->persistent_resume_attempt) {
        state->image_resume_platform_ready =
            payload->resume_platform_ready;
        state->image_resume_slot_present = payload->resume_slot_present;
        state->reviewed_image_resume_observation =
            payload->resume_observation_after_operation;
        state->image_resume_requested = payload->resume_slot_present &&
            payload->resume_observation_after_operation.has_value();
      }
      state->image_cancellation.reset();
      state->image_pause_controller.reset();
      SetWindowTextW(state->image_token, L"");
      SetWindowTextW(state->image_cancel, L"安全に取消");
      state->image_step =
          payload != nullptr && payload->success ? 4 : 1;
      if (payload != nullptr) {
        std::wstring text = payload->output;
        text += payload->success
            ? L"\r\n\r\n完成名へ確定済みです。復元前にも完全検証します。"
            : L"\r\n\r\n完了扱いにしていません。"
              L"コピー元はread-onlyのまま保護されている場合があります。";
        if (!payload->success && payload->persistent_resume_attempt &&
            payload->resume_slot_present) {
          text += payload->resume_observation_after_operation.has_value()
              ? L"\r\n確認済みのpartial/journalとactive.checkpointを保持しました。"
                L"同じSource・保存先を再選択し、パスワードを再入力して再開できます。"
              : L"\r\n再開情報の安全な再観測に失敗したため、再起動後の確認まで"
                L"新しい書込み操作を開始しないでください。";
        }
        SetWindowTextW(state->image_output, text.c_str());
      } else {
        SetWindowTextW(
            state->image_output,
            L"作成結果を受け取れませんでした。完了扱いにしていません。");
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
        static_cast<void>(offer_winpe_completion_power_action(
            *state,
            payload->completion_power_proof,
            L".tsumugiイメージ作成",
            payload->completion_power_target));
      }
      return 0;
    }
    case kImageRestoreProgressMessage: {
      std::unique_ptr<ImageRestoreProgressPayload> payload(
          reinterpret_cast<ImageRestoreProgressPayload*>(lparam));
      if (payload != nullptr && state->restore_progress_active) {
        state->restore_progress = payload->progress;
        state->restore_elapsed = payload->elapsed;
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    case kImageRestoreVerifyCompleteMessage: {
      std::unique_ptr<ImageRestoreVerifyPayload> payload(
          reinterpret_cast<ImageRestoreVerifyPayload*>(lparam));
      state->operation_busy = false;
      state->restore_progress_active = false;
      state->restore_write_active = false;
      state->restore_cancellation.reset();
      state->restore_pause_controller.reset();
      state->restore_progress = ytec::clonecore::DiskOperationProgress{};
      SetWindowTextW(state->restore_cancel, L"安全に取消");
      state->reviewed_restore_target.reset();
      state->reviewed_restore_target_identity.reset();
      state->reviewed_restore_target_layout_hash.reset();
      state->reviewed_restore_individual_partition.reset();
      state->reviewed_shrink_restore_work.reset();
      state->reviewed_shrink_restore_layout.reset();
      state->reviewed_shrink_original_source_target.reset();
      state->restore_execution_ready = false;
      SetWindowTextW(state->restore_token, L"");
      if (payload != nullptr && payload->selection.has_value() &&
          (!payload->selection->encrypted || payload->password != nullptr)) {
        state->verified_restore_image = std::move(payload->selection);
        populate_restore_source_partition_candidates(*state);
        state->reviewed_restore_image_password =
            std::move(payload->password);
        state->restore_step = 2;
        SetWindowTextW(state->restore_output, payload->output.c_str());
        SetFocus(state->restore_target);
      } else {
        state->verified_restore_image.reset();
        populate_restore_source_partition_candidates(*state);
        state->reviewed_restore_image_password.reset();
        state->restore_step = 1;
        SetWindowTextW(
            state->restore_output,
            payload == nullptr || payload->output.empty()
                ? L"イメージの完全検証結果を受け取れませんでした。"
                  L"対象ディスクは変更していません。"
                : payload->output.c_str());
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
      }
      return 0;
    }
    case kImageRestoreCompleteMessage: {
      std::unique_ptr<ImageRestorePayload> payload(
          reinterpret_cast<ImageRestorePayload*>(lparam));
      state->operation_busy = false;
      state->restore_progress_active = false;
      state->restore_write_active = false;
      state->restore_execution_ready = false;
      state->verified_restore_image.reset();
      state->reviewed_restore_image_password.reset();
      state->reviewed_restore_target.reset();
      state->reviewed_restore_target_identity.reset();
      state->reviewed_restore_target_layout_hash.reset();
      state->reviewed_restore_individual_partition.reset();
      state->reviewed_shrink_restore_work.reset();
      state->reviewed_shrink_restore_layout.reset();
      state->reviewed_shrink_original_source_target.reset();
      if (payload != nullptr && payload->persistent_resume_attempt) {
        state->restore_resume_platform_ready =
            payload->resume_platform_ready;
        state->restore_resume_slot_present =
            payload->resume_slot_present;
        state->reviewed_restore_resume_slot =
            payload->reviewed_resume_slot_after_operation;
        // A failed attempt may have retained either the pre-existing slot or
        // a newly published start checkpoint. Keep the product in resume mode
        // with the freshly inspected binding so the user can safely reselect
        // the same image/target without being forced to restart the GUI.
        state->restore_resume_requested = payload->resume_slot_present;
      }
      populate_restore_source_partition_candidates(*state);
      state->restore_cancellation.reset();
      state->restore_pause_controller.reset();
      state->restore_progress = ytec::clonecore::DiskOperationProgress{};
      SetWindowTextW(state->restore_token, L"");
      SetWindowTextW(state->restore_cancel, L"安全に取消");
      state->restore_step =
          payload != nullptr && payload->success ? 4 : 1;
      if (payload != nullptr) {
        std::wstring output = payload->output;
        if (!payload->success) {
          output +=
              L"\r\n\r\n完了扱いにしていません。"
              L"復元先はオフラインの未完了状態で保護されている可能性があります。";
        } else if (payload->partial_loss) {
          output +=
              L"\r\n\r\n一部欠損として完了しました。"
              L"復元先はオフラインのままです。起動成功は未確認です。";
        } else {
          output +=
              L"\r\n\r\n検証完了・オフライン保持です。"
              L"起動成功は未確認です。";
          if (payload->boot_repair_offer_required) {
            output += payload->shrink_restore
                ? L"\r\n縮小復元の最終配置には起動仕上げが必要です。"
                  L"「起動を修復」で対象ディスクを自動診断し、別の確認後に実行してください。"
                : L"\r\nWindowsを含む個別復元です。必要な場合だけ"
                  L"「起動を修復」で対象ディスクを自動診断し、別の確認後に実行してください。";
          }
        }
        SetWindowTextW(state->restore_output, output.c_str());
      } else {
        SetWindowTextW(
            state->restore_output,
            L"復元結果を受け取れませんでした。完了扱いにしていません。");
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
        static_cast<void>(offer_winpe_completion_power_action(
            *state,
            payload->completion_power_proof,
            L".tsumugiイメージ復元",
            payload->completion_power_target));
      }
      return 0;
    }
    case kBootInspectCompleteMessage: {
      std::unique_ptr<BootInspectPayload> payload(
          reinterpret_cast<BootInspectPayload*>(lparam));
      state->operation_busy = false;
      state->boot_execution_active = false;
      if (payload != nullptr && payload->plan.has_value() &&
          !payload->system_partition_creation.has_value() &&
          !payload->choices.has_value() &&
          !payload->execution.has_value() && payload->error.empty()) {
        if (payload->plan->system_partition_create_plan_needed) {
          const auto selected_windows =
              prompt_system_partition_creation_windows_choice(
                  window, payload->plan.value());
          if (selected_windows.has_value()) {
            auto plan = std::move(payload->plan.value());
            start_system_partition_creation_review(
                *state, std::move(plan), selected_windows.value());
          } else {
            state->repair_step = 1;
            state->inspected_boot_plan.reset();
            state->inspected_system_partition_creation.reset();
            state->inspected_boot_choices.reset();
            state->inspected_boot_execution.reset();
            state->inspected_efi_delete_plan.reset();
            state->inspected_boot_selections.clear();
            state->boot_confirmation_token.clear();
            std::wstring output = payload->output;
            output +=
                L"\r\n\r\nシステム領域作成の対象／追加確認を確定しなかったため、"
                L"ディスクは変更していません。";
            SetWindowTextW(state->repair_output, output.c_str());
            show_page_controls(*state);
            update_action_state(*state);
            InvalidateRect(window, nullptr, FALSE);
          }
          return 0;
        }
        const auto windows_choice =
            prompt_automatic_boot_repair_windows_choice(
                window, payload->plan.value());
        const auto efi_choice = windows_choice.has_value()
            ? prompt_automatic_boot_repair_efi_choice(
                  window, payload->plan.value(), windows_choice.value())
            : std::nullopt;
        const auto choice = efi_choice.has_value()
            ? prompt_automatic_boot_repair_nvram_choice(
                  window, payload->plan.value(), efi_choice.value())
            : std::nullopt;
        if (choice.has_value()) {
          auto plan = std::move(payload->plan.value());
          start_boot_review(*state, std::move(plan), choice.value());
        } else {
          state->repair_step = 1;
          state->inspected_boot_plan.reset();
          state->inspected_system_partition_creation.reset();
          state->inspected_boot_choices.reset();
          state->inspected_boot_execution.reset();
          state->inspected_efi_delete_plan.reset();
          state->inspected_boot_selections.clear();
          state->boot_confirmation_token.clear();
          std::wstring output = payload->output;
          output +=
              L"\r\n\r\nWindowsの登録対象／起動優先順を確定しなかったため、"
              L"起動修復は開始しません。ディスクは変更していません。";
          SetWindowTextW(state->repair_output, output.c_str());
          show_page_controls(*state);
          update_action_state(*state);
          InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
      }
      if (payload != nullptr &&
          payload->system_partition_creation.has_value() &&
          !payload->plan.has_value() && !payload->choices.has_value() &&
          !payload->execution.has_value() && payload->error.empty()) {
        state->inspected_boot_plan.reset();
        state->inspected_system_partition_creation =
            std::move(payload->system_partition_creation);
        state->inspected_boot_choices.reset();
        state->inspected_boot_execution.reset();
        state->inspected_efi_delete_plan.reset();
        state->inspected_boot_selections.clear();
        state->boot_confirmation_token = L"OK";
        state->repair_step = 3;
        std::wstring output = payload->output;
        output += L"\r\n\r\n最終確認語: OK";
        SetWindowTextW(state->repair_output, output.c_str());
        show_page_controls(*state);
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (payload != nullptr && payload->plan.has_value() &&
          payload->choices.has_value() &&
          payload->execution.has_value() &&
          !payload->selections.empty() &&
          (payload->execution->third_party_efi_delete_requested ==
           payload->efi_delete_plan.has_value())) {
        state->inspected_boot_plan = std::move(payload->plan);
        state->inspected_system_partition_creation.reset();
        state->inspected_boot_choices = std::move(payload->choices);
        state->inspected_boot_execution = std::move(payload->execution);
        state->inspected_efi_delete_plan =
            std::move(payload->efi_delete_plan);
        state->inspected_boot_selections =
            std::move(payload->selections);
        state->boot_confirmation_token = L"OK";
        state->repair_step = 3;
        std::wstring output = payload->output;
        output +=
            L"\r\n\r\n既存安全トランザクションの読み取り確認にも合格しました。"
            L"\r\n最終確認語: OK";
        SetWindowTextW(state->repair_output, output.c_str());
      } else {
        state->repair_step = 1;
        state->inspected_boot_plan.reset();
        state->inspected_system_partition_creation.reset();
        state->inspected_boot_choices.reset();
        state->inspected_boot_execution.reset();
        state->inspected_efi_delete_plan.reset();
        state->inspected_boot_selections.clear();
        state->boot_confirmation_token.clear();
        std::wstring output = payload == nullptr
            ? L"対象を安全に確定できませんでした。"
            : payload->output;
        if (payload != nullptr && !payload->error.empty()) {
          if (!output.empty()) {
            output += L"\r\n\r\n実行停止理由:\r\n";
          }
          output += payload->error;
        }
        SetWindowTextW(
            state->repair_output,
            output.empty()
                ? L"対象を安全に確定できませんでした。"
                : output.c_str());
      }
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
      }
      return 0;
    }
    case kWinReDiagnosticCompleteMessage: {
      std::unique_ptr<WinReDiagnosticPayload> payload(
          reinterpret_cast<WinReDiagnosticPayload*>(lparam));
      state->operation_busy = false;
      state->boot_execution_active = false;
      SetWindowTextW(
          state->repair_output,
          payload == nullptr || payload->output.empty()
              ? L"WinRE診断結果を受け取れませんでした。"
              : payload->output.c_str());
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
      }
      return 0;
    }
    case kBootExecuteCompleteMessage: {
      std::unique_ptr<BootExecutePayload> payload(
          reinterpret_cast<BootExecutePayload*>(lparam));
      state->operation_busy = false;
      state->boot_execution_active = false;
      if (payload != nullptr && payload->system_partition_creation_stage) {
        state->inspected_boot_plan.reset();
        state->inspected_system_partition_creation.reset();
        state->inspected_boot_choices.reset();
        state->inspected_boot_execution.reset();
        state->inspected_efi_delete_plan.reset();
        state->inspected_boot_selections.clear();
        state->boot_confirmation_token.clear();
        SendMessageW(
            state->repair_acknowledge,
            BM_SETCHECK,
            BST_UNCHECKED,
            0);
        SetWindowTextW(state->repair_token, L"");
        if (payload->success &&
            payload->completed_creation_plan.has_value()) {
          auto plan = std::move(payload->completed_creation_plan.value());
          SetWindowTextW(state->repair_output, payload->output.c_str());
          const auto windows_choice =
              prompt_automatic_boot_repair_windows_choice(window, plan);
          const auto efi_choice = windows_choice.has_value()
              ? prompt_automatic_boot_repair_efi_choice(
                    window, plan, windows_choice.value())
              : std::nullopt;
          const auto choice = efi_choice.has_value()
              ? prompt_automatic_boot_repair_nvram_choice(
                    window, plan, efi_choice.value())
              : std::nullopt;
          if (choice.has_value()) {
            start_boot_review(*state, std::move(plan), choice.value());
            return 0;
          }
          state->repair_step = 1;
          std::wstring output = payload->output;
          output +=
              L"\r\n\r\n新しいシステム領域は作成・読戻し済みです。"
              L"後続のWindows登録／NVRAM方針を確定しなかったため、"
              L"BCD、WinRE、NVRAMは変更していません。"
              L"自動解析から安全に再開できます。";
          SetWindowTextW(state->repair_output, output.c_str());
        } else {
          state->repair_step = 1;
          std::wstring output = payload->output;
          output += payload->partial
              ? L"\r\n\r\nrollback未確認のため、後続の起動修復を開始しません。"
              : L"\r\n\r\n後続の起動修復を開始していません。";
          SetWindowTextW(state->repair_output, output.c_str());
        }
        show_page_controls(*state);
        update_action_state(*state);
        InvalidateRect(window, nullptr, FALSE);
        show_captured_error(window, payload->error_presentation);
        return 0;
      }
      if (payload != nullptr) {
        state->repair_step = payload->success ? 4 : 1;
        std::wstring output = payload->output;
        if (!payload->success) {
          output += L"\r\n\r\n完了扱いにしていません。";
        }
        SetWindowTextW(state->repair_output, output.c_str());
      } else {
        state->repair_step = 1;
      }
      state->inspected_boot_plan.reset();
      state->inspected_system_partition_creation.reset();
      state->inspected_boot_choices.reset();
      state->inspected_boot_execution.reset();
      state->inspected_efi_delete_plan.reset();
      state->inspected_boot_selections.clear();
      state->boot_confirmation_token.clear();
      SendMessageW(
          state->repair_acknowledge,
          BM_SETCHECK,
          BST_UNCHECKED,
          0);
      SetWindowTextW(state->repair_token, L"");
      show_page_controls(*state);
      update_action_state(*state);
      InvalidateRect(window, nullptr, FALSE);
      if (payload != nullptr) {
        show_captured_error(window, payload->error_presentation);
        static_cast<void>(offer_winpe_completion_power_action(
            *state,
            payload->completion_power_proof,
            L"起動修復",
            payload->completion_power_target));
      }
      return 0;
    }
    case WM_CLOSE:
      if (state->operation_busy || state->inventory_busy) {
        if (state->clone_progress_active &&
            state->clone_progress.cancellation_allowed) {
          request_clone_cancellation(*state);
        } else if (state->image_progress_active &&
                   state->image_progress.cancellation_allowed) {
          request_image_cancellation(*state);
        } else if (state->restore_progress_active &&
                   state->restore_progress.cancellation_allowed) {
          request_restore_cancellation(*state);
        }
        MessageBoxW(
            window,
            state->clone_progress_active
                ? L"クローン処理が安全に停止または完了するまで画面を閉じられません。"
                : state->image_progress_active
                ? L"イメージ作成が安全に停止または完了するまで画面を閉じられません。"
                : state->restore_progress_active
                ? state->restore_write_active
                    ? L"イメージ復元が安全に停止または完了するまで画面を閉じられません。"
                    : L"イメージの完全検証が停止または完了するまで画面を閉じられません。"
                : state->boot_execution_active
                ? state->inspected_system_partition_creation.has_value()
                    ? L"システム領域作成トランザクションが完了するまで画面を閉じられません。"
                      L"確定処理は途中取消できません。"
                    : L"BCD安全トランザクションが完了するまで画面を閉じられません。"
                      L"確定処理は途中取消できません。"
                : L"実行中の確認処理が完了するまで画面を閉じられません。",
            L"処理中です",
            MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      destroy_state_resources(*state);
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      delete state;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE,
    _In_ PWSTR,
    _In_ int show_command) {
  // The GDI layout uses logical pixels; scale the complete surface uniformly.
  SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED);
  SetDefaultDllDirectories(
      LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
  INITCOMMONCONTROLSEX common_controls{
      .dwSize = sizeof(INITCOMMONCONTROLSEX),
      .dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES,
  };
  if (InitCommonControlsEx(&common_controls) == FALSE) {
    return 1;
  }

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    return 1;
  }

#if defined(YTEC_UI_ACCEPTANCE_BUILD)
  RECT desired{0, 0, 1024, 600};
#else
  RECT desired{0, 0, 1280, 720};
#endif
  AdjustWindowRectEx(
      &desired,
      WS_OVERLAPPEDWINDOW,
      FALSE,
      0);
  const int width = desired.right - desired.left;
  const int height = desired.bottom - desired.top;
  const int x = (std::max)(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
  const int y = (std::max)(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);

  const HWND window = CreateWindowExW(
      0,
      kWindowClass,
      kWindowTitle,
      WS_OVERLAPPEDWINDOW,
      x,
      y,
      width,
      height,
      nullptr,
      nullptr,
      instance,
      nullptr);
  if (window == nullptr) {
    return 1;
  }
  const bool desired_exceeds_display =
      width > GetSystemMetrics(SM_CXSCREEN) ||
      height > GetSystemMetrics(SM_CYSCREEN);
  ShowWindow(
      window,
      desired_exceeds_display ? SW_MAXIMIZE : show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    const HWND focus = GetFocus();
    const int focused_identifier =
        focus == nullptr ? 0 : GetDlgCtrlID(focus);
    const bool navigation_focused =
        focused_identifier == kNavCloneId ||
        focused_identifier == kNavImageCreateId ||
        focused_identifier == kNavImageRestoreId ||
        focused_identifier == kNavRepairId ||
        focused_identifier == kNavDiskId;
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

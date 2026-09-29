#include "ytec/windowsapp/adk_product_ui.h"

#include <Windows.h>
#include <Richedit.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr wchar_t kConsentClassName[] =
    L"YtecTsumugiAdkConsentDialog";
constexpr int kSummaryId = 5101;
constexpr int kEulaId = 5102;
constexpr int kAcceptanceId = 5103;
constexpr int kAcceptId = IDOK;
constexpr int kCancelId = IDCANCEL;
constexpr UINT_PTR kScrollTimerId = 1U;

clonecore::Error ui_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

struct RtfStreamState final {
  const std::vector<std::byte>* bytes{};
  std::size_t cursor{};
};

DWORD CALLBACK stream_rtf(
    const DWORD_PTR cookie,
    LPBYTE buffer,
    const LONG requested,
    LONG* const copied) {
  if (cookie == 0U || buffer == nullptr || copied == nullptr ||
      requested < 0) {
    return ERROR_INVALID_PARAMETER;
  }
  auto* const state =
      reinterpret_cast<RtfStreamState*>(cookie);
  if (state->bytes == nullptr || state->cursor > state->bytes->size()) {
    return ERROR_INVALID_DATA;
  }
  const std::size_t available = state->bytes->size() - state->cursor;
  const std::size_t count = (std::min)(
      available, static_cast<std::size_t>(requested));
  if (count > static_cast<std::size_t>((std::numeric_limits<LONG>::max)())) {
    return ERROR_ARITHMETIC_OVERFLOW;
  }
  if (count != 0U) {
    std::memcpy(buffer, state->bytes->data() + state->cursor, count);
  }
  state->cursor += count;
  *copied = static_cast<LONG>(count);
  return ERROR_SUCCESS;
}

struct ConsentDialogState final {
  const AdkReleaseManifest* manifest{};
  const AdkVerifiedEulaDocument* document{};
  std::wstring summary;
  HWND window{};
  HWND summary_control{};
  HWND eula_control{};
  HWND acceptance_control{};
  HWND accept_control{};
  HWND cancel_control{};
  bool eula_opened{};
  bool eula_end_reached{};
  bool closing{};
  std::optional<AdkConsentReviewAcknowledgement> acknowledgement;
  std::optional<clonecore::Error> error;
};

void set_dialog_font(const HWND control) {
  if (control != nullptr) {
    SendMessageW(
        control,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
  }
}

void update_accept_state(ConsentDialogState& state) {
  const bool checked = state.acceptance_control != nullptr &&
      SendMessageW(
          state.acceptance_control, BM_GETCHECK, 0, 0) == BST_CHECKED;
  if (state.accept_control != nullptr) {
    EnableWindow(
        state.accept_control,
        state.eula_opened && state.eula_end_reached && checked);
  }
  if (state.acceptance_control != nullptr) {
    SetWindowTextW(
        state.acceptance_control,
        state.eula_end_reached
            ? L"EULA全文を末尾まで読み、Microsoftの利用条件に同意します"
            : L"EULA全文を末尾まで確認すると同意を選択できます");
    EnableWindow(state.acceptance_control, state.eula_end_reached);
  }
}

void observe_eula_end(ConsentDialogState& state) {
  if (state.eula_control == nullptr || state.eula_end_reached) {
    return;
  }
  SCROLLINFO info{
      .cbSize = sizeof(SCROLLINFO),
      .fMask = SIF_PAGE | SIF_POS | SIF_RANGE,
  };
  if (GetScrollInfo(state.eula_control, SB_VERT, &info) == FALSE) {
    return;
  }
  const std::uint64_t page = info.nPage;
  const std::uint64_t position =
      static_cast<std::uint64_t>((std::max)(info.nPos, 0));
  const std::uint64_t maximum =
      static_cast<std::uint64_t>((std::max)(info.nMax, 0));
  if (maximum == 0U || page == 0U ||
      position + page > maximum) {
    state.eula_end_reached = true;
    update_accept_state(state);
  }
}

void layout_consent_controls(ConsentDialogState& state) {
  RECT client{};
  if (GetClientRect(state.window, &client) == FALSE) {
    return;
  }
  const auto layout = calculate_adk_consent_dialog_layout(
      client.right - client.left, client.bottom - client.top);
  MoveWindow(
      state.summary_control,
      layout.summary_left,
      layout.summary_top,
      layout.summary_width,
      layout.summary_height,
      TRUE);
  MoveWindow(
      state.eula_control,
      layout.eula_left,
      layout.eula_top,
      layout.eula_width,
      layout.eula_height,
      TRUE);
  MoveWindow(
      state.acceptance_control,
      layout.acceptance_left,
      layout.acceptance_top,
      layout.acceptance_width,
      layout.acceptance_height,
      TRUE);
  MoveWindow(
      state.accept_control,
      layout.accept_left,
      layout.accept_top,
      layout.accept_width,
      layout.accept_height,
      TRUE);
  MoveWindow(
      state.cancel_control,
      layout.cancel_left,
      layout.cancel_top,
      layout.cancel_width,
      layout.cancel_height,
      TRUE);
}

bool initialize_consent_controls(ConsentDialogState& state) {
  const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
      GetWindowLongPtrW(state.window, GWLP_HINSTANCE));
  state.summary_control = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      L"EDIT",
      state.summary.c_str(),
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
      0,
      0,
      0,
      0,
      state.window,
      reinterpret_cast<HMENU>(
          static_cast<INT_PTR>(kSummaryId)),
      instance,
      nullptr);
  state.eula_control = CreateWindowExW(
      WS_EX_CLIENTEDGE,
      MSFTEDIT_CLASS,
      L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
      0,
      0,
      0,
      0,
      state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEulaId)),
      instance,
      nullptr);
  state.acceptance_control = CreateWindowExW(
      0,
      L"BUTTON",
      L"EULA全文を末尾まで確認すると同意を選択できます",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX |
          BS_MULTILINE,
      0,
      0,
      0,
      0,
      state.window,
      reinterpret_cast<HMENU>(
          static_cast<INT_PTR>(kAcceptanceId)),
      instance,
      nullptr);
  state.accept_control = CreateWindowExW(
      0,
      L"BUTTON",
      L"同意して続行",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      0,
      0,
      0,
      0,
      state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAcceptId)),
      instance,
      nullptr);
  state.cancel_control = CreateWindowExW(
      0,
      L"BUTTON",
      L"キャンセル",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0,
      0,
      0,
      0,
      state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)),
      instance,
      nullptr);
  if (state.summary_control == nullptr || state.eula_control == nullptr ||
      state.acceptance_control == nullptr || state.accept_control == nullptr ||
      state.cancel_control == nullptr) {
    return false;
  }
  set_dialog_font(state.summary_control);
  set_dialog_font(state.eula_control);
  set_dialog_font(state.acceptance_control);
  set_dialog_font(state.accept_control);
  set_dialog_font(state.cancel_control);

  RtfStreamState stream_state{
      .bytes = &state.document->rtf_document,
  };
  EDITSTREAM stream{
      .dwCookie = reinterpret_cast<DWORD_PTR>(&stream_state),
      .dwError = ERROR_SUCCESS,
      .pfnCallback = stream_rtf,
  };
  const LRESULT streamed = SendMessageW(
      state.eula_control,
      EM_STREAMIN,
      SF_RTF,
      reinterpret_cast<LPARAM>(&stream));
  if (stream.dwError != ERROR_SUCCESS || streamed == 0 ||
      stream_state.cursor != state.document->rtf_document.size()) {
    return false;
  }
  SendMessageW(state.eula_control, EM_SETREADONLY, TRUE, 0);
  SendMessageW(state.eula_control, EM_SETSEL, 0, 0);
  SendMessageW(state.eula_control, EM_SCROLLCARET, 0, 0);
  state.eula_opened = true;
  EnableWindow(state.acceptance_control, FALSE);
  EnableWindow(state.accept_control, FALSE);
  layout_consent_controls(state);
  SetTimer(state.window, kScrollTimerId, 100U, nullptr);
  SetFocus(state.eula_control);
  return true;
}

void accept_consent(ConsentDialogState& state) {
  observe_eula_end(state);
  const bool checked = SendMessageW(
      state.acceptance_control, BM_GETCHECK, 0, 0) == BST_CHECKED;
  auto acknowledgement = complete_adk_consent_presentation(
      *state.manifest,
      *state.document,
      AdkConsentPresentationFacts{
          .official_sources_presented = true,
          .acquired_components_presented = true,
          .eula_body_opened = state.eula_opened,
          .eula_body_end_reached = state.eula_end_reached,
          .explicit_acceptance = checked,
      });
  if (!acknowledgement) {
    MessageBoxW(
        state.window,
        acknowledgement.error().message.c_str(),
        L"ADK利用条件の確認が未完了です",
        MB_OK | MB_ICONWARNING);
    return;
  }
  state.acknowledgement = acknowledgement.take_value();
  state.closing = true;
  DestroyWindow(state.window);
}

LRESULT CALLBACK consent_window_proc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
  auto* state = reinterpret_cast<ConsentDialogState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* const create =
        reinterpret_cast<const CREATESTRUCTW*>(lparam);
    state = reinterpret_cast<ConsentDialogState*>(create->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(
        window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }
  switch (message) {
    case WM_CREATE:
      if (!initialize_consent_controls(*state)) {
        state->error = ui_error(
            clonecore::ErrorCode::io_failed,
            GetLastError(),
            L"ADK EULA全文レビュー 画面生成",
            L"検証済みEULA全文を表示する製品画面を生成できませんでした");
        return -1;
      }
      return 0;
    case WM_SIZE:
      layout_consent_controls(*state);
      return 0;
    case WM_GETMINMAXINFO: {
      auto* const limits = reinterpret_cast<MINMAXINFO*>(lparam);
      limits->ptMinTrackSize.x = 660;
      limits->ptMinTrackSize.y = 520;
      return 0;
    }
    case WM_TIMER:
      if (wparam == kScrollTimerId) {
        observe_eula_end(*state);
      }
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == kAcceptanceId &&
          HIWORD(wparam) == BN_CLICKED) {
        update_accept_state(*state);
        return 0;
      }
      if (LOWORD(wparam) == kAcceptId &&
          HIWORD(wparam) == BN_CLICKED) {
        accept_consent(*state);
        return 0;
      }
      if (LOWORD(wparam) == kCancelId &&
          HIWORD(wparam) == BN_CLICKED) {
        state->closing = true;
        DestroyWindow(window);
        return 0;
      }
      break;
    case WM_CLOSE:
      state->closing = true;
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      KillTimer(window, kScrollTimerId);
      state->window = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

clonecore::Status ensure_consent_window_class(
    const HINSTANCE instance) {
  static ATOM window_class{};
  if (window_class != 0) {
    return clonecore::success_status();
  }
  WNDCLASSEXW definition{
      .cbSize = sizeof(WNDCLASSEXW),
      .style = CS_HREDRAW | CS_VREDRAW,
      .lpfnWndProc = consent_window_proc,
      .hInstance = instance,
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
      .lpszClassName = kConsentClassName,
  };
  window_class = RegisterClassExW(&definition);
  if (window_class == 0 && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    window_class = 1;
  }
  if (window_class == 0) {
    return clonecore::Status::failure(ui_error(
        clonecore::ErrorCode::io_failed,
        GetLastError(),
        L"ADK EULA全文レビュー window class",
        L"EULA全文レビュー画面を登録できませんでした"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::filesystem::path> select_adk_product_folder(
    const HWND owner,
    const std::wstring_view title) {
  IFileOpenDialog* dialog{};
  HRESULT result = CoCreateInstance(
      CLSID_FileOpenDialog,
      nullptr,
      CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&dialog));
  if (FAILED(result)) {
    return clonecore::Result<std::filesystem::path>::failure(ui_error(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(result),
        L"ADK管理 フォルダー選択",
        L"Windowsフォルダー選択画面を生成できませんでした"));
  }
  FILEOPENDIALOGOPTIONS options{};
  result = dialog->GetOptions(&options);
  if (SUCCEEDED(result)) {
    result = dialog->SetOptions(
        options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
        FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR);
  }
  const std::wstring title_text(title);
  if (SUCCEEDED(result)) {
    result = dialog->SetTitle(title_text.c_str());
  }
  if (SUCCEEDED(result)) {
    result = dialog->Show(owner);
  }
  IShellItem* item{};
  if (SUCCEEDED(result)) {
    result = dialog->GetResult(&item);
  }
  PWSTR selected{};
  if (SUCCEEDED(result)) {
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &selected);
  }
  std::filesystem::path path;
  if (SUCCEEDED(result) && selected != nullptr) {
    path = selected;
  }
  if (selected != nullptr) {
    CoTaskMemFree(selected);
  }
  if (item != nullptr) {
    item->Release();
  }
  dialog->Release();
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    return clonecore::Result<std::filesystem::path>::failure(ui_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"ADK管理 フォルダー選択",
        L"フォルダー選択をキャンセルしました。何も開始していません"));
  }
  if (FAILED(result) || path.empty()) {
    return clonecore::Result<std::filesystem::path>::failure(ui_error(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(result),
        L"ADK管理 フォルダー選択",
        L"ローカルフォルダーを選択できませんでした"));
  }
  return clonecore::Result<std::filesystem::path>::success(
      std::move(path));
}

clonecore::Result<AdkConsentReviewAcknowledgement>
show_adk_product_consent_dialog(
    const HWND owner,
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document) {
  const auto document_status =
      validate_adk_eula_document_for_presentation(manifest, document);
  if (!document_status) {
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(
        document_status.error());
  }
  const auto review = build_adk_consent_review(
      &manifest, &document.receipt);
  if (!review.ready_to_present) {
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(ui_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"ADK EULA全文レビュー 入力",
        review.message));
  }

  const HMODULE rich_edit = LoadLibraryW(L"Msftedit.dll");
  if (rich_edit == nullptr) {
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(ui_error(
        clonecore::ErrorCode::io_failed,
        GetLastError(),
        L"ADK EULA全文レビュー RichEdit",
        L"Windows RichEditを読み込めないためEULA全文を表示しません"));
  }
  const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
      GetModuleHandleW(nullptr));
  const auto registered = ensure_consent_window_class(instance);
  if (!registered) {
    FreeLibrary(rich_edit);
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(registered.error());
  }

  ConsentDialogState state{
      .manifest = &manifest,
      .document = &document,
      .summary = review.summary,
  };
  RECT owner_rect{};
  if (owner == nullptr || GetWindowRect(owner, &owner_rect) == FALSE) {
    owner_rect = RECT{0, 0, 1280, 720};
  }
  constexpr int width = 960;
  constexpr int height = 720;
  const int left = owner_rect.left +
      (owner_rect.right - owner_rect.left - width) / 2;
  const int top = owner_rect.top +
      (owner_rect.bottom - owner_rect.top - height) / 2;
  const HWND previous_focus = GetFocus();
  const HWND window = CreateWindowExW(
      WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
      kConsentClassName,
      L"Windows ADK 利用条件（全文確認）",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
      left,
      top,
      width,
      height,
      owner,
      nullptr,
      instance,
      &state);
  if (window == nullptr) {
    FreeLibrary(rich_edit);
    if (state.error.has_value()) {
      return clonecore::Result<
          AdkConsentReviewAcknowledgement>::failure(
          state.error.value());
    }
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(ui_error(
        clonecore::ErrorCode::io_failed,
        GetLastError(),
        L"ADK EULA全文レビュー 画面生成",
        L"検証済みEULA全文を表示する画面を開けませんでした"));
  }
  if (owner != nullptr) {
    EnableWindow(owner, FALSE);
  }
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);
  MSG message{};
  while (state.window != nullptr &&
         GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(window, &message) == FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (owner != nullptr) {
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
  }
  if (previous_focus != nullptr && IsWindow(previous_focus) != FALSE) {
    SetFocus(previous_focus);
  }
  FreeLibrary(rich_edit);
  if (state.error.has_value()) {
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(state.error.value());
  }
  if (!state.acknowledgement.has_value()) {
    return clonecore::Result<
        AdkConsentReviewAcknowledgement>::failure(ui_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"ADK EULA全文レビュー",
        L"利用条件への同意をキャンセルしました。取得・導入は開始していません"));
  }
  return clonecore::Result<
      AdkConsentReviewAcknowledgement>::success(
      std::move(state.acknowledgement.value()));
}

}  // namespace ytec::windowsapp

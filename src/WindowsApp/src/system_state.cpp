#include "ytec/windowsapp/system_state.h"

#include <Windows.h>
#include <wuapi.h>

namespace ytec::windowsapp {

PendingRestartObservation
query_windows_update_pending_restart() noexcept {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool owns_com_initialization =
      initialized == S_OK || initialized == S_FALSE;
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return PendingRestartObservation{
        .state = PendingRestartState::unknown,
        .native_status = static_cast<std::uint32_t>(initialized),
    };
  }

  ISystemInformation* information = nullptr;
  const HRESULT created = CoCreateInstance(
      CLSID_SystemInformation,
      nullptr,
      CLSCTX_INPROC_SERVER,
      IID_ISystemInformation,
      reinterpret_cast<void**>(&information));
  if (FAILED(created) || information == nullptr) {
    if (owns_com_initialization) {
      CoUninitialize();
    }
    return PendingRestartObservation{
        .state = PendingRestartState::unknown,
        .native_status = static_cast<std::uint32_t>(created),
    };
  }

  VARIANT_BOOL reboot_required = VARIANT_FALSE;
  const HRESULT queried = information->get_RebootRequired(&reboot_required);
  information->Release();
  if (owns_com_initialization) {
    CoUninitialize();
  }
  if (FAILED(queried)) {
    return PendingRestartObservation{
        .state = PendingRestartState::unknown,
        .native_status = static_cast<std::uint32_t>(queried),
    };
  }
  return PendingRestartObservation{
      .state = reboot_required == VARIANT_FALSE
          ? PendingRestartState::absent
          : PendingRestartState::required,
      .native_status = static_cast<std::uint32_t>(queried),
  };
}

std::wstring pending_restart_confirmation_note(
    const PendingRestartState state) {
  switch (state) {
    case PendingRestartState::absent:
      return {};
    case PendingRestartState::required:
      return
          L"\n\n注意: Windows Updateの完了に再起動が必要です。"
          L"\nバックアップ前にWindowsを再起動することを推奨します。";
    case PendingRestartState::unknown:
      return
          L"\n\n注意: Windows Updateの再起動保留状態を確認できませんでした。"
          L"\n保留中の更新がないことを確認してから進めてください。";
  }
  return
      L"\n\n注意: Windows Updateの再起動保留状態を確認できませんでした。";
}

}  // namespace ytec::windowsapp

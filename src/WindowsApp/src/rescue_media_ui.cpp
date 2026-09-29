#include "ytec/windowsapp/rescue_media_ui.h"

#include <sstream>
#include <utility>

namespace ytec::windowsapp {
namespace {

RescueMediaUsbUiView make_issue(
    const RescueMediaUsbUiIssue issue,
    std::wstring status) {
  return RescueMediaUsbUiView{
      .issue = issue,
      .selectors_visible = true,
      .status = std::move(status),
  };
}

std::wstring inspection_status_or(
    const std::wstring_view observed,
    const std::wstring_view fallback) {
  return observed.empty() ? std::wstring(fallback) : std::wstring(observed);
}

}  // namespace

RescueMediaUsbUiView build_rescue_media_usb_ui_view(
    const RescueMediaUsbUiInput& input) {
  if (!input.usb_kind_selected) {
    return {};
  }
  if (input.selected_target == nullptr) {
    return make_issue(
        RescueMediaUsbUiIssue::target_missing,
        L"作成先USBを選ぶと、所有情報と保持可否を読み取り専用で検査します。");
  }
  if (input.operation_running) {
    return make_issue(
        RescueMediaUsbUiIssue::inspection_scanning,
        L"レスキューメディアの処理中はUSB構成を変更できません。");
  }
  switch (input.inspection_state) {
    case RescueUsbInspectionState::not_started:
      return make_issue(
          RescueMediaUsbUiIssue::inspection_required,
          L"USBの所有情報と完全レイアウトを読み取り専用で検査してください。");
    case RescueUsbInspectionState::scanning:
      return make_issue(
          RescueMediaUsbUiIssue::inspection_scanning,
          inspection_status_or(
              input.inspection_message,
              L"USBの所有情報・起動領域・データ集約値を読み取り専用で検査しています。"));
    case RescueUsbInspectionState::blocked:
      return make_issue(
          RescueMediaUsbUiIssue::inspection_blocked,
          inspection_status_or(
              input.inspection_message,
              L"USBの所有情報を安全に判定できないため、書き込みを停止しています。"));
    case RescueUsbInspectionState::unknown_media:
    case RescueUsbInspectionState::verified_owned:
      break;
  }

  const bool owned =
      input.inspection_state == RescueUsbInspectionState::verified_owned;
  if (owned && input.owned_evidence == nullptr) {
    return make_issue(
        RescueMediaUsbUiIssue::inspection_blocked,
        L"検証済み媒体の所有証跡がないため、書き込みを停止しています。");
  }
  if (!owned && input.owned_evidence != nullptr) {
    return make_issue(
        RescueMediaUsbUiIssue::inspection_drift,
        L"USBの検査状態と所有証跡が一致しません。再検査してください。");
  }

  RescueMediaUsbUiView view;
  view.selectors_visible = true;
  view.recommended_mode = owned
      ? RescueUsbProvisioningMode::preserve_data_refresh
      : RescueUsbProvisioningMode::initialize_all;
  view.recommended_file_system = owned
      ? input.owned_evidence->cache_key.data_file_system
      : RescueUsbDataFileSystem::ntfs;
  view.mode_selector_enabled = owned;

  if (owned) {
    const auto binding = validate_rescue_usb_inspection_target_binding(
        *input.owned_evidence, *input.selected_target);
    if (!binding) {
      view.issue = RescueMediaUsbUiIssue::inspection_drift;
      view.status = binding.error().message.empty()
          ? L"USBの安定識別情報または完全レイアウトが検査時から変化しました。"
          : binding.error().message;
      return view;
    }
  }

  if (!input.selected_mode.has_value() ||
      !input.selected_file_system.has_value()) {
    view.issue = RescueMediaUsbUiIssue::selection_invalid;
    view.status = L"推奨された更新方法とデータ領域形式を確認してください。";
    return view;
  }
  const auto mode = input.selected_mode.value();
  const auto file_system = input.selected_file_system.value();
  if (!owned && mode != RescueUsbProvisioningMode::initialize_all) {
    view.issue = RescueMediaUsbUiIssue::selection_invalid;
    view.status =
        L"所有情報がないUSBは、全消去初期化だけを選択できます。";
    return view;
  }
  if (owned &&
      mode == RescueUsbProvisioningMode::preserve_data_refresh &&
      file_system != input.owned_evidence->cache_key.data_file_system) {
    view.issue = RescueMediaUsbUiIssue::selection_invalid;
    view.status =
        L"データ保持更新では既存データ領域の形式を変更できません。";
    return view;
  }

  view.file_system_selector_enabled =
      mode == RescueUsbProvisioningMode::initialize_all;
  const RescueUsbOwnedMediaInspection* owned_media =
      owned && mode == RescueUsbProvisioningMode::preserve_data_refresh
      ? &input.owned_evidence->owned_media
      : nullptr;
  auto storage_plan = plan_rescue_usb_storage({
      .target = input.selected_target,
      .mode = mode,
      .data_file_system = file_system,
      .owned_media = owned_media,
  });
  if (!storage_plan) {
    view.issue = RescueMediaUsbUiIssue::selection_invalid;
    view.status = storage_plan.error().message.empty()
        ? L"USBの安全な作成計画を構成できません。"
        : storage_plan.error().message;
    return view;
  }
  if (mode == RescueUsbProvisioningMode::preserve_data_refresh) {
    const auto evidence = validate_rescue_usb_inspection_evidence(
        *input.owned_evidence, *input.selected_target, mode, file_system);
    if (!evidence) {
      view.issue = RescueMediaUsbUiIssue::inspection_drift;
      view.status = evidence.error().message.empty()
          ? L"USBの検査証跡を現在の選択へ再照合できません。"
          : evidence.error().message;
      return view;
    }
  }

  view.issue = RescueMediaUsbUiIssue::ready;
  view.ready_for_review = true;
  view.confirmation_token = L"OK";
  view.storage_plan = storage_plan.take_value();
  std::wostringstream summary;
  summary << L"更新方法: " << rescue_usb_ui_mode_label(mode) << L'\n'
          << L"媒体構成: MBR／正確に4GiB FAT32起動領域＋残容量"
          << rescue_usb_data_file_system_name(file_system)
          << L"データ領域\n";
  if (mode == RescueUsbProvisioningMode::initialize_all) {
    summary << L"削除対象: USBディスク全体（既存パーティションを含む全内容）\n"
            << L"データ領域形式: "
            << rescue_usb_ui_file_system_label(file_system);
    view.status = owned
        ? L"検証済み媒体ですが、選択どおりUSB全体を消去して初期化します。"
        : L"所有情報がないUSBです。全消去初期化を推奨します。";
  } else {
    const auto& data =
        input.owned_evidence->owned_media.observed_data_tree_identity;
    summary << L"更新対象: 所有manifestで検証した起動／アプリ領域だけ（非上書き切替）\n"
            << L"保持対象: データ領域の全内容\n"
            << L"保持データ証跡: " << data.entry_count << L"件／"
            << data.total_logical_bytes << L" bytes";
    view.status =
        L"検証済みY-TEC媒体です。起動／アプリ領域だけを更新し、データ領域を完全保持します。";
  }
  view.review_summary = summary.str();
  return view;
}

std::wstring_view rescue_usb_ui_mode_label(
    const RescueUsbProvisioningMode mode) noexcept {
  switch (mode) {
    case RescueUsbProvisioningMode::initialize_all:
      return L"全消去初期化";
    case RescueUsbProvisioningMode::preserve_data_refresh:
      return L"起動更新（データ保持・推奨）";
  }
  return L"未対応";
}

std::wstring_view rescue_usb_ui_file_system_label(
    const RescueUsbDataFileSystem file_system) noexcept {
  switch (file_system) {
    case RescueUsbDataFileSystem::ntfs:
      return L"NTFS（既定）";
    case RescueUsbDataFileSystem::exfat:
      return L"exFAT（明示選択）";
  }
  return L"未対応";
}

}  // namespace ytec::windowsapp

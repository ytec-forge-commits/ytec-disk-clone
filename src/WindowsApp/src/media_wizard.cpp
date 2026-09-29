#include "ytec/windowsapp/media_wizard.h"

#include "ytec/diskmodel/clone_target_layout.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

bool is_ascii_letter(const wchar_t value) noexcept {
  return (value >= L'A' && value <= L'Z') ||
         (value >= L'a' && value <= L'z');
}

bool is_forbidden_path_character(const wchar_t value) noexcept {
  switch (value) {
    case L'"':
    case L'<':
    case L'>':
    case L'|':
    case L'?':
    case L'*':
    case L':':
      return true;
    default:
      return value < L' ';
  }
}

bool equals_ascii_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(
             left.begin(),
             left.end(),
             right.begin(),
             [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
             });
}

RescueMediaPlanView make_issue(
    const RescueMediaPlanIssue issue,
    const std::uint8_t current_step,
    std::wstring message) {
  return RescueMediaPlanView{
      .issue = issue,
      .current_step = current_step,
      .message = std::move(message),
  };
}

clonecore::Error usb_authorization_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"レスキューUSB実行直前確認",
      .message = std::move(message),
  };
}

std::wstring usb_confirmation_token() {
  return L"OK";
}

}  // namespace

bool is_safe_iso_destination_syntax(
    const std::wstring& path) noexcept {
  constexpr std::size_t kMaximumPathCharacters = 32U * 1024U - 1U;
  if (path.size() < 7U || path.size() > kMaximumPathCharacters ||
      !is_ascii_letter(path[0]) || path[1] != L':' ||
      path[2] != L'\\') {
    return false;
  }

  const std::wstring_view relative(path.data() + 3U, path.size() - 3U);
  if (relative.empty() || relative.front() == L'\\' ||
      relative.back() == L'\\' ||
      relative.find(L'/') != std::wstring_view::npos) {
    return false;
  }

  std::size_t component_start{};
  while (component_start < relative.size()) {
    const std::size_t separator =
        relative.find(L'\\', component_start);
    const std::size_t component_end =
        separator == std::wstring_view::npos
            ? relative.size()
            : separator;
    const std::wstring_view component =
        relative.substr(
            component_start, component_end - component_start);
    if (component.empty() || component == L"." ||
        component == L".." || component.back() == L'.' ||
        component.back() == L' ' ||
        std::any_of(
            component.begin(),
            component.end(),
            is_forbidden_path_character)) {
      return false;
    }
    if (separator == std::wstring_view::npos) {
      break;
    }
    component_start = separator + 1U;
  }

  const std::size_t last_separator = relative.rfind(L'\\');
  const std::wstring_view filename =
      last_separator == std::wstring_view::npos
          ? relative
          : relative.substr(last_separator + 1U);
  if (filename.size() <= 4U) {
    return false;
  }
  return equals_ascii_case_insensitive(
      filename.substr(filename.size() - 4U), L".iso");
}

RescueMediaPlanView evaluate_rescue_media_plan(
    const RescueMediaPlanInput& input) {
  if (input.preflight == nullptr) {
    return make_issue(
        RescueMediaPlanIssue::environment_check_required,
        1U,
        L"最初にADK／WinPEの作成環境を確認してください。");
  }
  if (!input.preflight->media_creation_permitted) {
    return make_issue(
        RescueMediaPlanIssue::environment_not_ready,
        1U,
        L"ADK／WinPEの診断項目が揃っていないため、安全側に停止しています。");
  }
  if (input.boot_profile ==
          RescueMediaBootProfile::windows_uefi_2023_ca &&
      !input.preflight->bootex_layout_ready) {
    return make_issue(
        RescueMediaPlanIssue::boot_profile_unavailable,
        2U,
        L"2023 CA用の構成を確認できません。互換性重視を選ぶか、ADKを更新してください。");
  }

  if (input.kind == RescueMediaKind::iso_file) {
    if (input.iso_destination.empty()) {
      return make_issue(
          RescueMediaPlanIssue::iso_destination_missing,
          2U,
          L"新しく作成するISOファイルの保存先を選んでください。");
    }
    if (!is_safe_iso_destination_syntax(input.iso_destination)) {
      return make_issue(
          RescueMediaPlanIssue::iso_destination_invalid,
          2U,
          L"ISOはローカルドライブ上の新しい絶対パス（.iso）を指定してください。");
    }

    RescueMediaPlanView view{
        .issue = RescueMediaPlanIssue::ready_for_confirmation,
        .current_step = 3U,
        .ready_for_confirmation = true,
        .message =
            L"ISOの作成内容を確認できます。まだファイルは作成していません。",
    };
    view.summary =
        L"出力形式: ISOファイル\n起動構成: " +
        rescue_media_boot_profile_label(input.boot_profile) +
        L"\n保存先: " + input.iso_destination +
        L"\n\n作業用ファイルはローカル一時領域だけに作成し、"
        L"完了後に削除します。Microsoft製ファイルは製品へ同梱しません。";
    return view;
  }

  if (input.inventory_loading) {
    return make_issue(
        RescueMediaPlanIssue::inventory_loading,
        2U,
        L"USB候補を読み取り専用で確認しています。");
  }
  if (input.inventory == nullptr) {
    return make_issue(
        RescueMediaPlanIssue::inventory_unavailable,
        2U,
        L"USB候補のディスク情報を取得できないため停止しています。");
  }
  if (!input.usb_target_index ||
      *input.usb_target_index >= input.inventory->disks.size()) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_not_selected,
        2U,
        L"作成先USBを選び、所有情報と保持可否を検査してください。");
  }

  const auto& target =
      input.inventory->disks[*input.usb_target_index];
  if (target.is_system_disk) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_is_system,
        2U,
        L"現在のWindowsディスクはUSB作成先にできません。");
  }
  if (!target.removable.has_value() ||
      !target.read_only.has_value() ||
      !target.offline.has_value()) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_state_unknown,
        2U,
        L"USBの取り外し可能・読み取り専用・オンライン状態を確認できません。");
  }
  if (!target.removable.value()) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_not_removable,
        2U,
        L"取り外し可能と確認できるUSBメモリだけを選択できます。");
  }
  if (!equals_ascii_case_insensitive(target.bus_type, L"USB")) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_not_usb,
        2U,
        L"接続方式がUSBと確認できるディスクだけを選択できます。");
  }
  if (target.read_only.value()) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_read_only,
        2U,
        L"選択したUSBは読み取り専用です。");
  }
  if (target.offline.value()) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_offline,
        2U,
        L"選択したUSBはオフラインです。Windowsでオンラインにして再確認してください。");
  }
  if (target.size_bytes < kRescueUsbMinimumBytes) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_too_small,
        2U,
        L"レスキューUSBは8GiB以上が必要です。16GiB以上を推奨します。");
  }
  auto identity = diskmodel::make_stable_disk_identity(target, false);
  if (!identity) {
    return make_issue(
        RescueMediaPlanIssue::usb_target_identity_unstable,
        2U,
        L"USBを安全に再識別できる情報が不足しています。");
  }
  if (input.reviewed_usb_storage_plan == nullptr) {
    return make_issue(
        RescueMediaPlanIssue::usb_inspection_required,
        2U,
        L"USBの所有情報・完全レイアウト検査が完了していません。");
  }
  const auto& reviewed_plan = *input.reviewed_usb_storage_plan;
  if (reviewed_plan.mode != input.usb_provisioning_mode ||
      reviewed_plan.data_file_system != input.usb_data_file_system) {
    return make_issue(
        RescueMediaPlanIssue::usb_inspection_required,
        2U,
        L"USBの検査済み計画と現在の更新方法／データ領域形式が一致しません。");
  }
  const auto storage_status = validate_rescue_usb_storage_plan(
      reviewed_plan, target, input.usb_owned_media);
  if (!storage_status) {
    return make_issue(
        reviewed_plan.mode ==
                RescueUsbProvisioningMode::preserve_data_refresh
            ? RescueMediaPlanIssue::usb_refresh_ownership_required
            : RescueMediaPlanIssue::usb_target_style_unknown,
        2U,
        storage_status.error().message.empty()
            ? L"USBの検査済み媒体計画を現在値へ再照合できません。"
            : storage_status.error().message);
  }

  RescueMediaPlanView view{
      .issue = RescueMediaPlanIssue::ready_for_confirmation,
      .current_step = 3U,
      .ready_for_confirmation = true,
      .usb_target_identity = reviewed_plan.expected_target,
      .usb_storage_plan = reviewed_plan,
      .confirmation_token = usb_confirmation_token(),
      .message =
          input.usb_provisioning_mode ==
                  RescueUsbProvisioningMode::preserve_data_refresh
              ? L"検証済みY-TEC媒体のデータ領域を保持する更新内容を確認できます。まだUSBは開いていません。"
              : L"選択USBを4GiB FAT32起動領域＋データ領域へ自動初期化する内容を確認できます。まだUSBは開いていません。",
  };
  std::wostringstream summary;
  summary << L"出力形式: USBメモリ（"
          << (input.usb_provisioning_mode ==
                      RescueUsbProvisioningMode::preserve_data_refresh
                  ? L"データ保持更新"
                  : L"全消去")
          << L"）\n"
          << L"起動構成: "
          << rescue_media_boot_profile_label(input.boot_profile)
          << L"\n対象: ディスク " << target.disk_number << L" / "
          << (target.model.empty() ? L"モデル不明" : target.model)
          << L" / " << target.size_bytes << L" bytes\n"
          << L"シリアル末尾: "
          << (target.serial_suffix.empty()
                  ? L"なし"
                  : std::wstring(
                        target.serial_suffix.begin(),
                        target.serial_suffix.end()))
          << (input.usb_provisioning_mode ==
                      RescueUsbProvisioningMode::preserve_data_refresh
                  ? L"\n保持対象: データ領域の全内容\n置換対象: 所有manifestで検証した起動／アプリ領域だけ（非上書き切替）\n"
                  : L"\n削除対象: USBディスク全体（既存パーティションを含む全内容）\n")
          << L"作成構成: MBR／正確に4GiB FAT32起動領域＋残容量"
          << rescue_usb_data_file_system_name(input.usb_data_file_system)
          << L"データ領域\n\n"
          << L"実行時は同じUSBを安定識別情報と完全レイアウトで再確認し、"
             L"二段階確認に合格するまで書き込みません。";
  if (input.usb_provisioning_mode ==
          RescueUsbProvisioningMode::preserve_data_refresh &&
      input.usb_owned_media != nullptr) {
    summary << L"\n保持データ証跡: "
            << input.usb_owned_media->observed_data_tree_identity.entry_count
            << L"件／"
            << input.usb_owned_media->observed_data_tree_identity
                   .total_logical_bytes
            << L" bytes";
  }
  view.summary = summary.str();
  return view;
}

clonecore::Result<RescueUsbTargetAuthorization>
authorize_rescue_usb_target(
    const RescueUsbAuthorizationRequest& request) {
  if (!request.reviewed_storage_plan.has_value()) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_INVALID_DATA,
            L"検査・レビュー済みの不変USB媒体計画がありません"));
  }
  if (request.fresh_inventory == nullptr) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::enumeration_failed,
            ERROR_INVALID_DATA,
            L"USB候補の再列挙結果がないため停止しました"));
  }
  if (!request.fresh_inventory->issues.empty()) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::enumeration_failed,
            ERROR_PARTIAL_COPY,
            L"物理ディスク再列挙に未解決の問題があるため、破壊的処理へ進みません"));
  }
  const auto expected_valid = clonecore::validate_stable_identity(
      request.expected_target,
      request.expected_target,
      L"選択済みUSB");
  if (!expected_valid || request.expected_target.is_system_disk) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        expected_valid
            ? usb_authorization_error(
                  clonecore::ErrorCode::unsupported_layout,
                  ERROR_ACCESS_DENIED,
                  L"選択済みUSBがシステムディスクを示しています")
            : expected_valid.error());
  }

  std::vector<std::size_t> matching_indices;
  for (std::size_t index = 0;
       index < request.fresh_inventory->disks.size();
       ++index) {
    const auto observed = diskmodel::make_stable_disk_identity(
        request.fresh_inventory->disks[index],
        request.fresh_inventory->disks[index].is_system_disk);
    if (observed &&
        clonecore::validate_stable_identity(
            request.expected_target,
            observed.value(),
            L"再列挙USB")) {
      matching_indices.push_back(index);
    }
  }
  if (matching_indices.size() != 1U) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::identity_mismatch,
            matching_indices.empty()
                ? ERROR_DEVICE_NOT_CONNECTED
                : ERROR_DUP_NAME,
            L"選択時と一致するUSBを再列挙結果から一意に特定できません"));
  }

  const std::size_t target_index = matching_indices.front();
  const auto& target =
      request.fresh_inventory->disks[target_index];
  if (target.disk_number != request.expected_target.disk_number) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"USBのディスク番号が選択時から変わったため、再選択が必要です"));
  }

  const MediaPreflightView preflight{
      .base_layout_ready = true,
      .bootex_layout_ready = true,
      .media_creation_permitted = true,
  };
  const auto plan = evaluate_rescue_media_plan({
      .preflight = &preflight,
      .kind = RescueMediaKind::usb_drive,
      .inventory = request.fresh_inventory,
      .usb_target_index = target_index,
      .usb_provisioning_mode =
          request.reviewed_storage_plan->mode,
      .usb_data_file_system =
          request.reviewed_storage_plan->data_file_system,
      .usb_owned_media = request.fresh_owned_media,
      .reviewed_usb_storage_plan =
          &request.reviewed_storage_plan.value(),
  });
  if (!plan.ready_for_confirmation ||
      !plan.usb_target_identity.has_value()) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_NOT_SUPPORTED,
            plan.message.empty()
                ? L"再列挙したUSBが安全条件を満たしません"
                : plan.message));
  }
  const auto identity = clonecore::validate_stable_identity(
      request.expected_target,
      plan.usb_target_identity.value(),
      L"レスキューUSB");
  if (!identity) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(identity.error());
  }
  const auto storage = validate_rescue_usb_storage_plan(
      request.reviewed_storage_plan.value(),
      target,
      request.fresh_owned_media);
  if (!storage) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(storage.error());
  }
  if (!request.first_step_acknowledged ||
      request.typed_confirmation != plan.confirmation_token) {
    return clonecore::Result<
        RescueUsbTargetAuthorization>::failure(
        usb_authorization_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            request.reviewed_storage_plan->mode ==
                    RescueUsbProvisioningMode::preserve_data_refresh
                ? L"データ保持更新の確認操作または確認語「OK」が一致しません"
                : L"USB全消去の確認操作または確認語「OK」が一致しません"));
  }

  return clonecore::Result<
      RescueUsbTargetAuthorization>::success({
      .target = plan.usb_target_identity.value(),
      .confirmation_token = plan.confirmation_token,
      .partition_count = target.partitions.size(),
      .storage_plan = request.reviewed_storage_plan,
      .physical_write_started = false,
  });
}

std::wstring rescue_media_kind_label(
    const RescueMediaKind kind) {
  switch (kind) {
    case RescueMediaKind::iso_file:
      return L"ISOファイル（推奨）";
    case RescueMediaKind::usb_drive:
      return L"USBメモリ（初期化／データ保持更新）";
  }
  return L"不明";
}

std::wstring rescue_media_boot_profile_label(
    const RescueMediaBootProfile profile) {
  switch (profile) {
    case RescueMediaBootProfile::windows_uefi_2011_ca:
      return L"BIOS／UEFI・Secure Boot 2011 CA（互換性重視）";
    case RescueMediaBootProfile::windows_uefi_2023_ca:
      return L"BIOS／UEFI・Secure Boot 2023 CA（最新PC向け）";
  }
  return L"不明";
}

}  // namespace ytec::windowsapp

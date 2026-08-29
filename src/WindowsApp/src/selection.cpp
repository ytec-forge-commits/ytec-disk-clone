#include "ytec/windowsapp/selection.h"

#include "ytec/diskmodel/clone_target_layout.h"

#include <array>

namespace ytec::windowsapp {

namespace {

constexpr std::array<WindowsTransferModeOption, 3U> kCloneTransferModes{{
    {WindowsTransferModeChoice::exact,
     L"通常モード（完全複製）"},
    {WindowsTransferModeChoice::shrink, L"縮小移行モード（小容量へ）"},
    {WindowsTransferModeChoice::rescue, L"救出モード（データディスク）"},
}};

constexpr std::array<WindowsTransferModeOption, 3U> kImageTransferModes{{
    {WindowsTransferModeChoice::exact,
     L"通常モード（完全複製）"},
    {WindowsTransferModeChoice::shrink, L"縮小移行モード（小容量へ）"},
    {WindowsTransferModeChoice::rescue, L"救出モード（データディスク）"},
}};

constexpr std::array<WindowsPartitionStyleOption, 2U>
    kPartitionStyleChoices{{
        {WindowsPartitionStyleChoice::preserve,
         L"パーティション形式: 維持"},
        {WindowsPartitionStyleChoice::mbr_to_gpt,
         L"パーティション形式: MBR→GPT"},
    }};

}  // namespace

std::span<const WindowsTransferModeOption> windows_transfer_mode_options(
    const WindowsTransferModeContext context) noexcept {
  switch (context) {
    case WindowsTransferModeContext::clone:
      return kCloneTransferModes;
    case WindowsTransferModeContext::create_image:
      return kImageTransferModes;
    default:
      return {};
  }
}

std::uintptr_t windows_transfer_mode_item_data(
    const WindowsTransferModeChoice choice) noexcept {
  return static_cast<std::uintptr_t>(choice);
}

std::optional<WindowsTransferModeChoice>
decode_windows_transfer_mode_item_data(const std::uintptr_t item_data) noexcept {
  switch (item_data) {
    case static_cast<std::uintptr_t>(
        WindowsTransferModeChoice::exact):
      return WindowsTransferModeChoice::exact;
    case static_cast<std::uintptr_t>(WindowsTransferModeChoice::shrink):
      return WindowsTransferModeChoice::shrink;
    case static_cast<std::uintptr_t>(WindowsTransferModeChoice::rescue):
      return WindowsTransferModeChoice::rescue;
    default:
      return std::nullopt;
  }
}

bool windows_transfer_mode_allowed(
    const WindowsTransferModeContext context,
    const WindowsTransferModeChoice choice) noexcept {
  const auto options = windows_transfer_mode_options(context);
  for (const auto& option : options) {
    if (option.choice == choice) {
      return true;
    }
  }
  return false;
}

bool windows_transfer_mode_requires_same_or_larger_target(
    const WindowsTransferModeChoice choice) noexcept {
  switch (choice) {
    case WindowsTransferModeChoice::shrink:
      return false;
    case WindowsTransferModeChoice::exact:
    case WindowsTransferModeChoice::rescue:
    default:
      return true;
  }
}

std::span<const WindowsPartitionStyleOption>
windows_partition_style_options() noexcept {
  return kPartitionStyleChoices;
}

std::uintptr_t windows_partition_style_item_data(
    const WindowsPartitionStyleChoice choice) noexcept {
  return static_cast<std::uintptr_t>(choice);
}

std::optional<WindowsPartitionStyleChoice>
decode_windows_partition_style_item_data(
    const std::uintptr_t item_data) noexcept {
  switch (item_data) {
    case static_cast<std::uintptr_t>(WindowsPartitionStyleChoice::preserve):
      return WindowsPartitionStyleChoice::preserve;
    case static_cast<std::uintptr_t>(WindowsPartitionStyleChoice::mbr_to_gpt):
      return WindowsPartitionStyleChoice::mbr_to_gpt;
    default:
      return std::nullopt;
  }
}

bool windows_partition_style_choice_allowed(
    const WindowsTransferModeChoice transfer_mode,
    const diskmodel::PartitionStyle source_style,
    const bool source_is_system_disk,
    const WindowsPartitionStyleChoice style_choice) noexcept {
  switch (transfer_mode) {
    case WindowsTransferModeChoice::exact:
      return style_choice == WindowsPartitionStyleChoice::preserve;
    case WindowsTransferModeChoice::shrink:
      return style_choice == WindowsPartitionStyleChoice::preserve ||
          (style_choice == WindowsPartitionStyleChoice::mbr_to_gpt &&
           source_is_system_disk &&
           source_style == diskmodel::PartitionStyle::mbr);
    case WindowsTransferModeChoice::rescue:
      return style_choice == WindowsPartitionStyleChoice::preserve;
    default:
      return false;
  }
}

bool windows_partition_style_route_available(
    const WindowsTransferModeChoice transfer_mode,
    const diskmodel::PartitionStyle source_style,
    const bool source_is_system_disk,
    const WindowsPartitionStyleChoice style_choice) noexcept {
  if (!windows_partition_style_choice_allowed(
          transfer_mode,
          source_style,
          source_is_system_disk,
          style_choice)) {
    return false;
  }
  if (transfer_mode == WindowsTransferModeChoice::shrink &&
      style_choice == WindowsPartitionStyleChoice::preserve) {
    return source_style == diskmodel::PartitionStyle::gpt ||
        source_style == diskmodel::PartitionStyle::mbr;
  }
  return true;
}

CloneSelectionView evaluate_clone_selection(
    const diskmodel::InventoryReport* const inventory,
    const std::optional<std::size_t> source_index,
    const std::optional<std::size_t> target_index,
    const bool inventory_loading,
    const bool require_target_same_or_larger) {
  if (inventory_loading) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::loading,
        .message = L"ディスクを読み取り専用で確認しています…"};
  }
  if (inventory == nullptr || inventory->disks.empty()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::inventory_unavailable,
        .message = L"利用できるディスク情報がありません。"};
  }
  if (!source_index.has_value() || !target_index.has_value() ||
      source_index.value() >= inventory->disks.size() ||
      target_index.value() >= inventory->disks.size()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::disk_not_selected,
        .message = L"コピー元とコピー先を選択してください。"};
  }
  if (source_index.value() == target_index.value()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::same_disk,
        .message = L"同じディスクをコピー元とコピー先には指定できません。"};
  }

  const auto& source = inventory->disks[source_index.value()];
  const auto& target = inventory->disks[target_index.value()];
  if (target.is_system_disk) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_is_system,
        .message =
            L"現在動作中のWindowsディスクはコピー先にできません。"};
  }
  if (!target.read_only.has_value()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_state_unknown,
        .message =
            L"コピー先の読み取り専用状態を確認できないため進めません。"};
  }
  if (target.read_only.value()) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_is_read_only,
        .message = L"コピー先ディスクが読み取り専用です。"};
  }
  if (target.removable.value_or(false) &&
      _wcsicmp(target.bus_type.c_str(), L"USB") == 0) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_is_usb_memory,
        .message =
            L"USBメモリはクローン先にできません。USB接続のHDD／SSDは選択できます。"};
  }
  if (diskmodel::disk_health_operation_advice(target.health, false) ==
      diskmodel::DiskHealthOperationAdvice::block_target) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_health_abnormal,
        .message =
            L"コピー先のSMART／NVMe健康状態が注意または異常のため開始できません。"};
  }
  const auto target_layout =
      diskmodel::classify_clone_target_layout(target);
  if (target_layout == diskmodel::CloneTargetLayoutKind::unsupported) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_layout_unsupported,
        .message =
            L"不明・動的・Storage Spacesの構成はコピー先にできません。"};
  }
  if (require_target_same_or_larger &&
      target.size_bytes < source.size_bytes) {
    return CloneSelectionView{
        .issue = CloneSelectionIssue::target_too_small,
        .message =
            L"コピー先の容量がコピー元より小さいため進めません。"};
  }
  return CloneSelectionView{
      .issue = CloneSelectionIssue::ready,
      .ready = true,
      .target_requires_initialization =
          target_layout ==
          diskmodel::CloneTargetLayoutKind::supported_initialized,
      .message = target_layout ==
              diskmodel::CloneTargetLayoutKind::supported_initialized
          ? L"フォーマット済みコピー先です。実行直前に再識別し、OK確認後に全領域を自動初期化します。"
          : L"選択内容は安全確認へ進めます。"};
}

}  // namespace ytec::windowsapp

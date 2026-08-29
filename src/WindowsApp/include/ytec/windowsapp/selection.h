#pragma once

#include "ytec/diskmodel/disk_inventory.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

// The Windows product reuses one native combo box on the clone and image-
// creation pages.  Choices are therefore carried as explicit item data and
// validated against the active page context; display indexes are never mode
// identifiers.
enum class WindowsTransferModeChoice : std::uintptr_t {
  exact = 1U,
  shrink = 2U,
  rescue = 3U,
};

enum class WindowsTransferModeContext : std::uint8_t {
  clone,
  create_image,
};

struct WindowsTransferModeOption final {
  WindowsTransferModeChoice choice{WindowsTransferModeChoice::exact};
  std::wstring_view label;
};

[[nodiscard]] std::span<const WindowsTransferModeOption>
windows_transfer_mode_options(WindowsTransferModeContext context) noexcept;

[[nodiscard]] std::uintptr_t windows_transfer_mode_item_data(
    WindowsTransferModeChoice choice) noexcept;

[[nodiscard]] std::optional<WindowsTransferModeChoice>
decode_windows_transfer_mode_item_data(std::uintptr_t item_data) noexcept;

[[nodiscard]] bool windows_transfer_mode_allowed(
    WindowsTransferModeContext context,
    WindowsTransferModeChoice choice) noexcept;

[[nodiscard]] bool windows_transfer_mode_requires_same_or_larger_target(
    WindowsTransferModeChoice choice) noexcept;

// Partition-style conversion is orthogonal to normal/shrink transfer mode.
// It is a clone-page-only selector and is never inferred from a transfer-mode
// display index.
enum class WindowsPartitionStyleChoice : std::uintptr_t {
  preserve = 1U,
  mbr_to_gpt = 2U,
};

struct WindowsPartitionStyleOption final {
  WindowsPartitionStyleChoice choice{
      WindowsPartitionStyleChoice::preserve};
  std::wstring_view label;
};

[[nodiscard]] std::span<const WindowsPartitionStyleOption>
windows_partition_style_options() noexcept;

[[nodiscard]] std::uintptr_t windows_partition_style_item_data(
    WindowsPartitionStyleChoice choice) noexcept;

[[nodiscard]] std::optional<WindowsPartitionStyleChoice>
decode_windows_partition_style_item_data(std::uintptr_t item_data) noexcept;

[[nodiscard]] bool windows_partition_style_choice_allowed(
    WindowsTransferModeChoice transfer_mode,
    diskmodel::PartitionStyle source_style,
    bool source_is_system_disk,
    WindowsPartitionStyleChoice style_choice) noexcept;

// Product execution remains narrower than selector visibility, but both GPT
// and basic-primary MBR preservation are wired for shrink. Detailed MBR
// eligibility is still proved by read-only analysis before any target I/O.
[[nodiscard]] bool windows_partition_style_route_available(
    WindowsTransferModeChoice transfer_mode,
    diskmodel::PartitionStyle source_style,
    bool source_is_system_disk,
    WindowsPartitionStyleChoice style_choice) noexcept;

enum class CloneSelectionIssue : std::uint8_t {
  loading,
  inventory_unavailable,
  transfer_mode_unavailable,
  partition_style_unavailable,
  partition_style_route_unavailable,
  disk_not_selected,
  same_disk,
  target_is_system,
  target_is_read_only,
  target_state_unknown,
  target_is_usb_memory,
  target_health_abnormal,
  target_layout_unsupported,
  target_too_small,
  ready,
};

struct CloneSelectionView final {
  CloneSelectionIssue issue{CloneSelectionIssue::inventory_unavailable};
  bool ready{};
  bool target_requires_initialization{};
  std::wstring message;
};

[[nodiscard]] CloneSelectionView evaluate_clone_selection(
    const diskmodel::InventoryReport* inventory,
    std::optional<std::size_t> source_index,
    std::optional<std::size_t> target_index,
    bool inventory_loading,
    bool require_target_same_or_larger = true);

}  // namespace ytec::windowsapp

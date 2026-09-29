#pragma once

#include "ytec/clonecore/operation_progress.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ytec::windowsapp {

enum class OperationStage : std::uint8_t {
  waiting,
  preflight,
  snapshot,
  reading,
  writing,
  verifying,
  boot_finalization,
  completed,
  failed,
};

struct ProgressInput final {
  OperationStage stage{OperationStage::waiting};
  std::uint64_t processed_bytes{};
  std::uint64_t total_bytes{};
  std::chrono::milliseconds elapsed{};
  bool cancellation_allowed{};
};

struct ProgressView final {
  double fraction{};
  std::uint64_t bytes_per_second{};
  std::optional<std::chrono::seconds> remaining;
  std::wstring stage_label;
  std::wstring processed_label;
  std::wstring speed_label;
  std::wstring elapsed_label;
  std::wstring remaining_label;
  bool cancellation_allowed{};
};

struct OnlineImageProgressView final {
  double fraction{};
  std::wstring percentage_label;
  std::wstring stage_label;
  std::wstring read_label;
  std::wstring write_label;
  std::wstring verified_label;
  std::wstring speed_label;
  std::wstring elapsed_label;
  std::wstring remaining_label;
  bool cancellation_allowed{};
  bool pause_allowed{};
};

[[nodiscard]] ProgressView calculate_progress(const ProgressInput& input);

[[nodiscard]] OnlineImageProgressView build_online_image_progress_view(
    const clonecore::DiskOperationProgress& progress,
    std::chrono::milliseconds elapsed);

[[nodiscard]] std::wstring format_bytes(std::uint64_t bytes);

[[nodiscard]] std::wstring format_duration(std::chrono::seconds duration);

}  // namespace ytec::windowsapp

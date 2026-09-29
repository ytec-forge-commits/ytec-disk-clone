#include "ytec/windowsapp/progress.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace ytec::windowsapp {
namespace {

constexpr std::uint64_t kMinimumEtaBytes = 16ULL * 1024ULL * 1024ULL;
constexpr auto kMinimumEtaElapsed = std::chrono::seconds(3);
constexpr std::array<std::wstring_view, 5> kByteUnits{
    L"B", L"KiB", L"MiB", L"GiB", L"TiB"};

std::uint64_t saturating_add(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
  return right > (std::numeric_limits<std::uint64_t>::max)() - left
      ? (std::numeric_limits<std::uint64_t>::max)()
      : left + right;
}

std::wstring_view stage_label(const OperationStage stage) noexcept {
  switch (stage) {
    case OperationStage::waiting:
      return L"開始前";
    case OperationStage::preflight:
      return L"安全確認";
    case OperationStage::snapshot:
      return L"VSSスナップショット作成";
    case OperationStage::reading:
      return L"データを読み取り中";
    case OperationStage::writing:
      return L"コピー先へ書き込み中";
    case OperationStage::verifying:
      return L"読み戻し検証中";
    case OperationStage::boot_finalization:
      return L"起動構成を仕上げ中";
    case OperationStage::completed:
      return L"完了";
    case OperationStage::failed:
      return L"停止";
  }
  return L"不明";
}

std::wstring_view online_image_stage_label(
    const clonecore::DiskOperationStage stage) noexcept {
  switch (stage) {
    case clonecore::DiskOperationStage::planning:
      return L"VSSと保存先を安全確認中";
    case clonecore::DiskOperationStage::copying_data:
      return L"Snapshotから読み取り、イメージへ保存中";
    case clonecore::DiskOperationStage::verifying_source:
      return L"作成したイメージを読み戻し検証中";
    case clonecore::DiskOperationStage::flushing_data:
      return L"VSSを完了し、最終ファイル名へ確定中";
    case clonecore::DiskOperationStage::completed:
      return L"イメージ作成完了";
    case clonecore::DiskOperationStage::invalidating_target:
    case clonecore::DiskOperationStage::staging_partition_table:
    case clonecore::DiskOperationStage::committing_partition_table:
    case clonecore::DiskOperationStage::validating_conversion:
    case clonecore::DiskOperationStage::converting_partition_style:
    case clonecore::DiskOperationStage::rebuilding_boot:
    case clonecore::DiskOperationStage::verifying_final:
      return L"安全な最終処理中";
  }
  return L"処理中";
}

std::wstring byte_progress_label(
    const std::uint64_t processed,
    const std::uint64_t total) {
  if (total == 0) {
    return processed == 0 ? L"—" : format_bytes(processed);
  }
  std::size_t unit = 0;
  std::uint64_t divisor = 1;
  while (total / divisor >= 1024U && unit + 1U < kByteUnits.size()) {
    divisor *= 1024U;
    ++unit;
  }
  const auto number = [divisor](const std::uint64_t bytes) {
    if (bytes == 0) {
      return std::wstring(L"0");
    }
    const long double value =
        static_cast<long double>(bytes) /
        static_cast<long double>(divisor);
    if (value < 0.1L) {
      return std::wstring(L"<0.1");
    }
    std::wostringstream stream;
    if (bytes % divisor == 0U || value >= 100.0L) {
      stream << std::fixed << std::setprecision(0) << value;
    } else {
      stream << std::fixed << std::setprecision(1) << value;
    }
    return stream.str();
  };
  return number((std::min)(processed, total)) + L" / " + number(total) +
      L" " + std::wstring(kByteUnits.at(unit));
}

}  // namespace

std::wstring format_bytes(const std::uint64_t bytes) {
  long double value = static_cast<long double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0L && unit + 1 < kByteUnits.size()) {
    value /= 1024.0L;
    ++unit;
  }

  std::wostringstream stream;
  if (unit == 0) {
    stream << bytes;
  } else {
    stream << std::fixed << std::setprecision(value >= 100.0L ? 0 : 1)
           << value;
  }
  stream << L" " << kByteUnits[unit];
  return stream.str();
}

std::wstring format_duration(std::chrono::seconds duration) {
  if (duration.count() < 0) {
    duration = std::chrono::seconds::zero();
  }
  const auto hours =
      std::chrono::duration_cast<std::chrono::hours>(duration);
  duration -= hours;
  const auto minutes =
      std::chrono::duration_cast<std::chrono::minutes>(duration);
  duration -= minutes;

  std::wostringstream stream;
  if (hours.count() > 0) {
    stream << hours.count() << L"時間";
  }
  if (minutes.count() > 0 || hours.count() > 0) {
    stream << minutes.count() << L"分";
  }
  stream << duration.count() << L"秒";
  return stream.str();
}

ProgressView calculate_progress(const ProgressInput& input) {
  const std::uint64_t bounded_processed =
      input.total_bytes == 0
      ? input.processed_bytes
      : (std::min)(input.processed_bytes, input.total_bytes);
  const double fraction =
      input.total_bytes == 0
      ? 0.0
      : static_cast<double>(bounded_processed) /
            static_cast<double>(input.total_bytes);

  std::uint64_t bytes_per_second = 0;
  if (input.elapsed.count() > 0) {
    const long double rate =
        static_cast<long double>(bounded_processed) * 1000.0L /
        static_cast<long double>(input.elapsed.count());
    bytes_per_second = rate >=
            static_cast<long double>(
                (std::numeric_limits<std::uint64_t>::max)())
        ? (std::numeric_limits<std::uint64_t>::max)()
        : static_cast<std::uint64_t>(rate);
  }

  std::optional<std::chrono::seconds> remaining;
  if (input.total_bytes > bounded_processed &&
      bounded_processed >= kMinimumEtaBytes &&
      input.elapsed >= kMinimumEtaElapsed &&
      bytes_per_second > 0) {
    const std::uint64_t remaining_bytes =
        input.total_bytes - bounded_processed;
    const std::uint64_t seconds =
        remaining_bytes / bytes_per_second +
        (remaining_bytes % bytes_per_second == 0 ? 0ULL : 1ULL);
    remaining = std::chrono::seconds(seconds);
  }

  const std::wstring processed =
      input.total_bytes == 0
      ? format_bytes(bounded_processed)
      : format_bytes(bounded_processed) + L" / " +
            format_bytes(input.total_bytes);
  return ProgressView{
      .fraction = fraction,
      .bytes_per_second = bytes_per_second,
      .remaining = remaining,
      .stage_label = std::wstring(stage_label(input.stage)),
      .processed_label = processed,
      .speed_label =
          bytes_per_second == 0
          ? L"測定中"
          : format_bytes(bytes_per_second) + L"/秒",
      .elapsed_label =
          format_duration(
              std::chrono::duration_cast<std::chrono::seconds>(
                  input.elapsed)),
      .remaining_label =
          remaining.has_value()
          ? format_duration(remaining.value())
          : input.stage == OperationStage::completed
          ? L"0秒"
          : input.stage == OperationStage::waiting ||
                    input.stage == OperationStage::preflight ||
                    input.stage == OperationStage::failed
          ? L"—"
          : L"計算中",
      .cancellation_allowed = input.cancellation_allowed,
  };
}

OnlineImageProgressView build_online_image_progress_view(
    const clonecore::DiskOperationProgress& progress,
    const std::chrono::milliseconds elapsed) {
  const std::uint64_t bounded_read =
      progress.total_read_bytes == 0
      ? progress.read_bytes
      : (std::min)(progress.read_bytes, progress.total_read_bytes);
  const std::uint64_t bounded_write =
      progress.total_write_bytes == 0
      ? progress.written_bytes
      : (std::min)(progress.written_bytes, progress.total_write_bytes);
  const std::uint64_t bounded_verify =
      progress.total_verify_bytes == 0
      ? progress.verified_bytes
      : (std::min)(progress.verified_bytes, progress.total_verify_bytes);
  const std::uint64_t processed_work = saturating_add(
      saturating_add(bounded_read, bounded_write), bounded_verify);
  const std::uint64_t total_work = saturating_add(
      saturating_add(
          progress.total_read_bytes, progress.total_write_bytes),
      progress.total_verify_bytes);

  double fraction =
      total_work == 0
      ? 0.0
      : static_cast<double>(processed_work) /
            static_cast<double>(total_work);
  if (progress.stage == clonecore::DiskOperationStage::completed) {
    fraction = 1.0;
  }
  fraction = (std::clamp)(fraction, 0.0, 1.0);

  std::uint64_t bytes_per_second = 0;
  if (elapsed.count() > 0) {
    const long double rate =
        static_cast<long double>(processed_work) * 1000.0L /
        static_cast<long double>(elapsed.count());
    bytes_per_second =
        rate >= static_cast<long double>(
                    (std::numeric_limits<std::uint64_t>::max)())
        ? (std::numeric_limits<std::uint64_t>::max)()
        : static_cast<std::uint64_t>(rate);
  }

  std::optional<std::chrono::seconds> remaining;
  const bool measurable_stage =
      progress.stage == clonecore::DiskOperationStage::copying_data ||
      progress.stage == clonecore::DiskOperationStage::verifying_source;
  if (measurable_stage && total_work > processed_work &&
      processed_work >= kMinimumEtaBytes &&
      elapsed >= kMinimumEtaElapsed && bytes_per_second > 0) {
    const std::uint64_t remaining_work = total_work - processed_work;
    remaining = std::chrono::seconds(
        remaining_work / bytes_per_second +
        (remaining_work % bytes_per_second == 0 ? 0ULL : 1ULL));
  }

  std::wstring remaining_label;
  if (progress.stage == clonecore::DiskOperationStage::completed) {
    remaining_label = L"0秒";
  } else if (
      progress.stage == clonecore::DiskOperationStage::flushing_data ||
      progress.stage ==
          clonecore::DiskOperationStage::staging_partition_table ||
      progress.stage ==
          clonecore::DiskOperationStage::committing_partition_table ||
      progress.stage ==
          clonecore::DiskOperationStage::validating_conversion ||
      progress.stage ==
          clonecore::DiskOperationStage::converting_partition_style ||
      progress.stage == clonecore::DiskOperationStage::rebuilding_boot ||
      progress.stage == clonecore::DiskOperationStage::verifying_final) {
    remaining_label = L"仕上げ中";
  } else if (remaining.has_value()) {
    remaining_label = format_duration(remaining.value());
  } else if (measurable_stage) {
    remaining_label = L"計算中";
  } else {
    remaining_label = L"—";
  }

  std::wostringstream percentage;
  percentage << static_cast<unsigned int>(fraction * 100.0 + 0.5)
             << L"%";
  return OnlineImageProgressView{
      .fraction = fraction,
      .percentage_label = percentage.str(),
      .stage_label =
          std::wstring(online_image_stage_label(progress.stage)),
      .read_label =
          byte_progress_label(progress.read_bytes, progress.total_read_bytes),
      .write_label = byte_progress_label(
          progress.written_bytes, progress.total_write_bytes),
      .verified_label = byte_progress_label(
          progress.verified_bytes, progress.total_verify_bytes),
      .speed_label =
          bytes_per_second == 0
          ? L"測定中"
          : format_bytes(bytes_per_second) + L"/秒",
      .elapsed_label = format_duration(
          std::chrono::duration_cast<std::chrono::seconds>(elapsed)),
      .remaining_label = std::move(remaining_label),
      .cancellation_allowed = progress.cancellation_allowed,
      .pause_allowed = progress.pause_allowed,
  };
}

}  // namespace ytec::windowsapp

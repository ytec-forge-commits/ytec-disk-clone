#include "ytec/winpeapp/dashboard.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace ytec::winpeapp {
namespace {

constexpr std::array<std::wstring_view, 5> kCapacityUnits{
    L"B", L"KiB", L"MiB", L"GiB", L"TiB"};

std::wstring partition_style_text(
    const diskmodel::PartitionStyle style) {
  switch (style) {
    case diskmodel::PartitionStyle::raw:
      return L"RAW";
    case diskmodel::PartitionStyle::mbr:
      return L"MBR";
    case diskmodel::PartitionStyle::gpt:
      return L"GPT";
    case diskmodel::PartitionStyle::unknown:
      return L"不明";
  }
  return L"不明";
}

std::wstring tri_state_text(
    const std::optional<bool>& value,
    const std::wstring_view yes,
    const std::wstring_view no) {
  if (!value.has_value()) {
    return L"不明";
  }
  return std::wstring(value.value() ? yes : no);
}

std::wstring serial_text(const std::string& suffix) {
  if (suffix.empty()) {
    return L"不明";
  }
  std::wstring result;
  result.reserve(suffix.size() + 1U);
  result.push_back(L'…');
  for (const unsigned char character : suffix) {
    result.push_back(static_cast<wchar_t>(character));
  }
  return result;
}

bool is_known_safe_target_candidate(const diskmodel::DiskInfo& disk) {
  return !disk.is_system_disk && disk.partition_style !=
             diskmodel::PartitionStyle::unknown &&
         disk.offline.has_value() && disk.read_only.has_value() &&
         disk.removable.has_value() && !disk.read_only.value() &&
         !disk.removable.value() &&
         diskmodel::disk_health_operation_advice(disk.health, false) !=
             diskmodel::DiskHealthOperationAdvice::block_target;
}

std::wstring health_text_impl(const diskmodel::DiskHealthInfo& health) {
  std::wstring text = std::wstring(
      diskmodel::disk_health_state_name(health.state));
  if (health.temperature_celsius.has_value()) {
    text += L" / " +
            std::to_wstring(health.temperature_celsius.value()) +
            L"°C";
  } else {
    text += L" / 温度未取得";
  }
  if (health.temperature_warning) {
    text += L"（温度警告）";
  }
  return text;
}

std::wstring issue_text(const diskmodel::InventoryIssue& issue) {
  std::wstring text = issue.device.empty() ? L"ディスク列挙" : issue.device;
  text += L": ";
  text += issue.error.operation;
  if (!issue.error.message.empty()) {
    text += L" — ";
    text += issue.error.message;
  }
  return text;
}

std::wstring_view operation_stage_text(
    const clonecore::DiskOperationStage stage) noexcept {
  switch (stage) {
    case clonecore::DiskOperationStage::planning:
      return L"安全なコピー計画を確認中";
    case clonecore::DiskOperationStage::verifying_source:
      return L"復元イメージを完全検証中";
    case clonecore::DiskOperationStage::invalidating_target:
      return L"コピー先を未完了状態に設定中";
    case clonecore::DiskOperationStage::copying_data:
      return L"データをコピーして読戻し検証中";
    case clonecore::DiskOperationStage::flushing_data:
      return L"書込み内容を確実に保存中";
    case clonecore::DiskOperationStage::staging_partition_table:
      return L"パーティション情報を仮配置中";
    case clonecore::DiskOperationStage::committing_partition_table:
      return L"パーティション情報を最終確定中";
    case clonecore::DiskOperationStage::validating_conversion:
      return L"Microsoft MBR2GPTで変換条件を検証中";
    case clonecore::DiskOperationStage::converting_partition_style:
      return L"コピー先をGPT / UEFI構成へ変換中";
    case clonecore::DiskOperationStage::rebuilding_boot:
      return L"UEFI起動ファイルとBCDを再構築中";
    case clonecore::DiskOperationStage::verifying_final:
      return L"変換後の構成と起動情報を最終確認中";
    case clonecore::DiskOperationStage::completed:
      return L"完了";
  }
  return L"処理中";
}

std::wstring format_duration(std::chrono::seconds duration) {
  if (duration < std::chrono::seconds::zero()) {
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

std::wstring byte_progress_label(
    const std::uint64_t processed,
    const std::uint64_t total) {
  if (total == 0) {
    return processed == 0 ? L"—" : format_dashboard_capacity(processed);
  }
  std::size_t unit = 0;
  std::uint64_t divisor = 1;
  while (total / divisor >= 1024U &&
         unit + 1U < kCapacityUnits.size()) {
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
      L" " + std::wstring(kCapacityUnits.at(unit));
}

}  // namespace

std::wstring format_dashboard_health(const diskmodel::DiskInfo& disk) {
  return format_dashboard_health(disk.health);
}

std::wstring format_dashboard_health(
    const diskmodel::DiskHealthInfo& health) {
  return health_text_impl(health);
}

std::wstring format_dashboard_capacity(const std::uint64_t bytes) {
  long double value = static_cast<long double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0L && unit + 1U < kCapacityUnits.size()) {
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
  stream << L" " << kCapacityUnits[unit];
  return stream.str();
}

OperationProgressView build_operation_progress_view(
    const clonecore::DiskOperationProgress& progress,
    const std::chrono::milliseconds elapsed) {
  const std::uint64_t bounded_verified =
      progress.total_verify_bytes == 0
      ? progress.verified_bytes
      : (std::min)(progress.verified_bytes, progress.total_verify_bytes);
  double fraction =
      progress.total_verify_bytes == 0
      ? 0.0
      : static_cast<double>(bounded_verified) /
            static_cast<double>(progress.total_verify_bytes);
  if (progress.stage == clonecore::DiskOperationStage::completed) {
    fraction = 1.0;
  }

  std::uint64_t bytes_per_second = 0;
  if (elapsed.count() > 0) {
    const long double rate =
        static_cast<long double>(bounded_verified) * 1000.0L /
        static_cast<long double>(elapsed.count());
    bytes_per_second =
        rate >= static_cast<long double>(
                    (std::numeric_limits<std::uint64_t>::max)())
        ? (std::numeric_limits<std::uint64_t>::max)()
        : static_cast<std::uint64_t>(rate);
  }

  constexpr std::uint64_t kMinimumEtaBytes =
      16ULL * 1024ULL * 1024ULL;
  constexpr auto kMinimumEtaElapsed = std::chrono::seconds(3);
  std::optional<std::chrono::seconds> remaining;
  if ((progress.stage ==
           clonecore::DiskOperationStage::verifying_source ||
       progress.stage ==
           clonecore::DiskOperationStage::copying_data) &&
      progress.total_verify_bytes > bounded_verified &&
      bounded_verified >= kMinimumEtaBytes &&
      elapsed >= kMinimumEtaElapsed && bytes_per_second > 0) {
    const std::uint64_t remaining_bytes =
        progress.total_verify_bytes - bounded_verified;
    const std::uint64_t seconds =
        remaining_bytes / bytes_per_second +
        (remaining_bytes % bytes_per_second == 0 ? 0ULL : 1ULL);
    remaining = std::chrono::seconds(seconds);
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
  } else if (
      progress.stage ==
          clonecore::DiskOperationStage::verifying_source ||
      progress.stage ==
          clonecore::DiskOperationStage::copying_data) {
    remaining_label = L"計算中";
  } else {
    remaining_label = L"—";
  }

  std::wostringstream percentage;
  percentage << static_cast<unsigned int>(
      (std::clamp)(fraction, 0.0, 1.0) * 100.0 + 0.5)
             << L"%";
  return OperationProgressView{
      .fraction = (std::clamp)(fraction, 0.0, 1.0),
      .percentage_label = percentage.str(),
      .stage_label = std::wstring(operation_stage_text(progress.stage)),
      .partition_label =
          progress.partition_index.has_value()
          ? L"パーティション #" +
                std::to_wstring(progress.partition_index.value())
          : L"ディスク全体",
      .read_label = byte_progress_label(
          progress.read_bytes, progress.total_read_bytes),
      .write_label = byte_progress_label(
          progress.written_bytes, progress.total_write_bytes),
      .verified_label = byte_progress_label(
          progress.verified_bytes, progress.total_verify_bytes),
      .speed_label =
          bytes_per_second == 0
          ? L"測定中"
          : format_dashboard_capacity(bytes_per_second) + L"/秒",
      .elapsed_label = format_duration(
          std::chrono::duration_cast<std::chrono::seconds>(elapsed)),
      .remaining_label = std::move(remaining_label),
      .cancellation_allowed = progress.cancellation_allowed,
      .pause_allowed = progress.pause_allowed,
  };
}

DashboardView build_dashboard_view(
    const diskmodel::InventoryReport& inventory) {
  DashboardView view;
  view.readiness = DashboardReadiness::ready;
  view.headline = L"安全に作業を始められます";
  view.guidance =
      L"コピー元とコピー先をこのPEで直接選択してください。";
  view.direct_clone_available = !inventory.disks.empty();
  view.boot_repair_review_available = !inventory.disks.empty();

  view.disks.reserve(inventory.disks.size());
  bool has_health_warning = false;
  for (const auto& disk : inventory.disks) {
    has_health_warning = has_health_warning ||
        disk.health.state == diskmodel::DiskHealthState::caution ||
        disk.health.state == diskmodel::DiskHealthState::failing ||
        disk.health.temperature_warning;
    DashboardDiskView disk_view;
    disk_view.disk_number = disk.disk_number;
    disk_view.system_disk = disk.is_system_disk;
    disk_view.selectable_as_target = is_known_safe_target_candidate(disk);

    std::wostringstream title;
    title << L"ディスク " << disk.disk_number << L"  "
          << (disk.model.empty() ? L"モデル不明" : disk.model);
    disk_view.title = title.str();

    std::wostringstream list_label;
    list_label << L"ディスク " << disk.disk_number << L"  ·  "
               << format_dashboard_capacity(disk.size_bytes) << L"  "
               << (disk.model.empty() ? L"モデル不明" : disk.model)
               << L"  ·  健康 " << format_dashboard_health(disk);
    disk_view.list_label = list_label.str();

    std::wostringstream summary;
    summary << format_dashboard_capacity(disk.size_bytes) << L"  ·  "
            << partition_style_text(disk.partition_style) << L"  ·  "
             << (disk.bus_type.empty() ? L"Bus不明" : disk.bus_type)
             << L"  ·  健康 " << format_dashboard_health(disk);
    if (disk.is_system_disk) {
      summary << L"  ·  現在の起動環境";
    }
    disk_view.summary = summary.str();

    std::wostringstream details;
    details << disk_view.title << L"\r\n"
            << L"容量: " << format_dashboard_capacity(disk.size_bytes)
            << L"\r\n"
            << L"論理 / 物理セクター: " << disk.logical_sector_size
            << L" / " << disk.physical_sector_size << L" バイト\r\n"
            << L"接続: "
            << (disk.bus_type.empty() ? L"不明" : disk.bus_type)
            << L"\r\n"
            << L"シリアル末尾: " << serial_text(disk.serial_suffix)
            << L"\r\n"
            << L"形式: " << partition_style_text(disk.partition_style)
            << L"\r\n"
             << L"健康状態: " << format_dashboard_health(disk) << L"\r\n"
            << L"オフライン: "
            << tri_state_text(disk.offline, L"はい", L"いいえ")
            << L" / 読み取り専用: "
            << tri_state_text(disk.read_only, L"はい", L"いいえ")
            << L" / 取り外し可能: "
            << tri_state_text(disk.removable, L"はい", L"いいえ")
            << L"\r\n"
            << L"パーティション: " << disk.partitions.size() << L" 個";
    for (const auto& partition : disk.partitions) {
      details << L"\r\n  #" << partition.number << L"  "
              << format_dashboard_capacity(partition.size_bytes) << L"  "
              << (partition.name.empty() ? partition.type : partition.name);
      if (partition.bootable) {
        details << L"  [起動]";
      }
    }
    disk_view.details = details.str();
    view.disks.push_back(std::move(disk_view));
  }

  view.diagnostics.reserve(inventory.issues.size());
  for (const auto& issue : inventory.issues) {
    view.diagnostics.push_back(issue_text(issue));
  }
  if (!inventory.issues.empty()) {
    view.readiness = DashboardReadiness::warning;
    view.headline = L"確認できないディスク情報があります";
    view.guidance =
        L"診断内容を確認してください。不明な対象への実行は許可しません。";
    view.direct_clone_available = false;
    view.boot_repair_review_available = false;
  } else if (inventory.disks.empty()) {
    view.readiness = DashboardReadiness::blocked;
    view.headline = L"物理ディスクを確認できません";
    view.guidance =
        L"接続を確認して再読込みしてください。処理は開始できません。";
    view.direct_clone_available = false;
    view.boot_repair_review_available = false;
  } else if (has_health_warning) {
    view.readiness = DashboardReadiness::warning;
    view.headline = L"ディスクの健康状態を確認してください";
    view.guidance =
        L"注意・異常のディスクはコピー先にできません。コピー元なら救出モードを推奨します。"
        L"温度警告だけでは自動停止しませんが、冷却と電源状態を確認してください。";
  }
  return view;
}

}  // namespace ytec::winpeapp

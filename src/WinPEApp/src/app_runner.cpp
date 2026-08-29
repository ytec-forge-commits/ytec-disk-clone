#include "ytec/winpeapp/app_runner.h"

#include "ytec/clitools/cli_runner.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/diskmodel/inventory_formatter.h"
#include "ytec/winpeapp/clone_execution_readiness.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr clitools::InventoryCliPresentation kWinPePresentation{
    .title = "Y-TEC Tsumugi Drive WinPE ディスク診断（読み取り専用）",
    .executable_name = "ytec-winpe-app",
};

enum class OutputFormat : std::uint8_t {
  text,
  json,
};

struct DirectArguments final {
  std::uint32_t source_disk_number{};
  std::uint32_t target_disk_number{};
  OutputFormat format{OutputFormat::text};
};

clonecore::Error app_error(
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

bool is_help_argument(const std::wstring_view argument) {
  return argument == L"--help" || argument == L"-h" || argument == L"/?";
}

std::optional<std::uint32_t> parse_disk_number(const std::wstring_view text) {
  if (text.empty() || text.size() > 10U) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    value = value * 10U + static_cast<std::uint64_t>(character - L'0');
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
      return std::nullopt;
    }
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<DirectArguments> parse_direct_arguments(
    const std::vector<std::wstring>& arguments) {
  if (arguments.empty()) {
    return std::nullopt;
  }
  if (arguments.front() != L"--clone-preflight") {
    return std::nullopt;
  }

  std::optional<std::uint32_t> source;
  std::optional<std::uint32_t> target;
  OutputFormat format = OutputFormat::text;
  bool format_selected = false;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const auto& argument = arguments[index];
    if (argument == L"--source" || argument == L"--target") {
      if (index + 1U >= arguments.size()) {
        return std::nullopt;
      }
      const auto number = parse_disk_number(arguments[++index]);
      if (!number.has_value()) {
        return std::nullopt;
      }
      auto& destination = argument == L"--source" ? source : target;
      if (destination.has_value()) {
        return std::nullopt;
      }
      destination = number;
      continue;
    }
    if (argument == L"--text" || argument == L"--json") {
      const auto requested = argument == L"--json"
                                 ? OutputFormat::json
                                 : OutputFormat::text;
      if (format_selected && requested != format) {
        return std::nullopt;
      }
      format = requested;
      format_selected = true;
      continue;
    }
    return std::nullopt;
  }

  if (!source.has_value() || !target.has_value() || source == target) {
    return std::nullopt;
  }
  return DirectArguments{
      .source_disk_number = source.value(),
      .target_disk_number = target.value(),
      .format = format,
  };
}

void write_usage(std::ostream& stream) {
  stream
      << "Y-TEC Tsumugi Drive WinPE 読取り専用診断\n"
         "使い方:\n"
         "  ytec-winpe-app [--text | --json]\n"
         "  ytec-winpe-app --clone-preflight --source N --target N "
         "[--text | --json]\n"
         "  ytec-winpe-app --help\n\n"
         "このCLIは列挙とクローン事前確認だけを行い、ディスクへ書き込みません。\n"
         "破壊的な操作は、製品GUIで対象要約を確認し、大文字 OK を手入力した\n"
         "同じセッション内からだけ開始できます。\n"
         "予約ファイルの検索、読込み、変換、実行、削除は行いません。\n";
}

clonecore::Status validate_clone_execution_report(
    const CloneExecutionReport& report,
    const diskmodel::PartitionStyle expected_style) {
  const bool style_matches =
      (expected_style == diskmodel::PartitionStyle::gpt &&
       report.partition_style == ClonePartitionStyle::gpt) ||
      (expected_style == diskmodel::PartitionStyle::mbr &&
       report.partition_style == ClonePartitionStyle::mbr);
  const bool boot_finalization_valid =
      !report.boot_finalization_required ||
      (report.boot_repair.bcdboot.microsoft_signature_verified &&
       report.boot_repair.bcdboot.exit_code == 0U &&
       report.boot_repair.boot_store_verified &&
       (!report.boot_repair.system_partition_temporarily_mounted ||
        report.boot_repair.temporary_mount_released) &&
       report.temporary_mounts_released &&
       report.boot_finalization_verified);
  if (!style_matches || report.copied_data_bytes == 0U ||
      report.copied_partition_count == 0U || !report.read_back_verified ||
      !report.partition_table_committed ||
      (report.target_returned_online == report.target_left_offline) ||
      !boot_finalization_valid) {
    return clonecore::Status::failure(app_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE直接クローンの実行結果",
        L"形式、読戻し、パーティション確定、最終ディスク状態、または起動再構築を完全確認できません"));
  }
  return clonecore::success_status();
}

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t lhs, const wchar_t rhs) {
            return std::towupper(lhs) == std::towupper(rhs);
          });
}

std::wstring canonical_partition_type(const std::wstring_view type) {
  std::wstring canonical(type);
  std::transform(
      canonical.begin(),
      canonical.end(),
      canonical.begin(),
      [](const wchar_t value) {
        return value >= L'a' && value <= L'z'
            ? static_cast<wchar_t>(value - L'a' + L'A')
            : value;
      });
  return canonical;
}

void append_u8(
    std::vector<std::byte>& material,
    const std::uint8_t value) {
  material.push_back(static_cast<std::byte>(value));
}

void append_u32(
    std::vector<std::byte>& material,
    const std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    material.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(
    std::vector<std::byte>& material,
    const std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    material.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_narrow_text(
    std::vector<std::byte>& material,
    const std::string_view value) {
  append_u64(material, static_cast<std::uint64_t>(value.size()));
  for (const char character : value) {
    material.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
}

void append_wide_text(
    std::vector<std::byte>& material,
    const std::wstring_view value) {
  append_u64(material, static_cast<std::uint64_t>(value.size()));
  for (const wchar_t character : value) {
    append_u32(material, static_cast<std::uint32_t>(character));
  }
}

void append_identity(
    std::vector<std::byte>& material,
    const clonecore::StableDiskIdentity& identity) {
  append_u32(material, identity.disk_number);
  append_wide_text(material, identity.model);
  append_u64(material, identity.size_bytes);
  append_u32(material, identity.logical_sector_size);
  append_narrow_text(material, identity.serial_suffix);
  append_wide_text(material, identity.device_instance_id);
  append_u8(material, identity.is_system_disk ? 1U : 0U);
}

void append_layout(
    std::vector<std::byte>& material,
    const Mbr2GptCanonicalDiskLayout& layout) {
  append_u8(
      material, static_cast<std::uint8_t>(layout.disk_style));
  append_u64(
      material, static_cast<std::uint64_t>(layout.partitions.size()));
  for (const auto& partition : layout.partitions) {
    append_u32(material, partition.number);
    append_u8(material, static_cast<std::uint8_t>(partition.style));
    append_wide_text(material, partition.type);
    append_u64(material, partition.offset_bytes);
    append_u64(material, partition.size_bytes);
    append_u8(material, partition.bootable ? 1U : 0U);
  }
}

clonecore::Result<imageformat::Sha256Digest>
calculate_mbr2gpt_review_binding(
    const Mbr2GptDirectOperationPlan& plan) {
  std::vector<std::byte> material;
  material.reserve(1024U);
  append_narrow_text(material, "YTEC-WINPE-MBR2GPT-REVIEW-V1");
  append_identity(material, plan.clone.expected_source);
  append_identity(material, plan.clone.expected_target);
  append_u8(
      material,
      static_cast<std::uint8_t>(plan.clone.source_partition_style));
  append_wide_text(material, plan.clone.source_bus_type);
  append_wide_text(material, plan.clone.target_bus_type);
  append_u64(
      material,
      static_cast<std::uint64_t>(plan.clone.source_partition_count));
  append_u64(
      material,
      static_cast<std::uint64_t>(plan.clone.target_partition_count));
  append_u64(
      material,
      static_cast<std::uint64_t>(plan.primary_partition_count));
  append_u32(material, plan.active_system_partition_number);
  append_layout(material, plan.source_layout);
  append_layout(material, plan.target_layout);
  return imageformat::sha256(material);
}

clonecore::Status validate_mbr2gpt_source_layout(
    const diskmodel::DiskInfo& source) {
  if (source.partition_style != diskmodel::PartitionStyle::mbr ||
      source.partitions.empty() || source.partitions.size() > 3U ||
      source.logical_sector_size != 512U ||
      source.size_bytes / source.logical_sector_size >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::uint32_t>::max)()) + 1ULL) {
    return clonecore::Status::failure(app_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE MBRからGPT移行のコピー元構成",
        L"512バイト論理セクターで、1～3個の基本プライマリ領域を持つMBRディスクだけを対象にします"));
  }
  if (diskmodel::disk_health_operation_advice(source.health, true) ==
      diskmodel::DiskHealthOperationAdvice::recommend_rescue) {
    return clonecore::Status::failure(app_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_DEVICE_HARDWARE_ERROR,
        L"PE MBRからGPT移行のコピー元健康状態",
        L"コピー元に注意・異常があるため、形式変換と排他的な救出モードを使用してください"));
  }

  std::vector<const diskmodel::PartitionInfo*> ordered;
  ordered.reserve(source.partitions.size());
  std::size_t active_system_count = 0U;
  for (const auto& partition : source.partitions) {
    const bool supported_type =
        equals_case_insensitive(partition.type, L"0x07") ||
        equals_case_insensitive(partition.type, L"0x27");
    if (partition.style != diskmodel::PartitionStyle::mbr ||
        partition.number == 0U || partition.number > 4U ||
        partition.offset_bytes < source.logical_sector_size ||
        partition.size_bytes == 0U || !supported_type ||
        partition.offset_bytes > source.size_bytes ||
        partition.size_bytes > source.size_bytes - partition.offset_bytes) {
      return clonecore::Status::failure(app_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"PE MBRからGPT移行の基本パーティション",
          L"現在は範囲内の基本NTFS/Windows回復プライマリ領域だけを変換対象にできます"));
    }
    if (partition.bootable) {
      if (!equals_case_insensitive(partition.type, L"0x07")) {
        return clonecore::Status::failure(app_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"PE MBRからGPT移行のActive領域",
            L"Active領域は基本NTFSとして一意に確認できる必要があります"));
      }
      ++active_system_count;
    }
    if (std::any_of(
            ordered.begin(),
            ordered.end(),
            [&](const diskmodel::PartitionInfo* existing) {
              return existing->number == partition.number;
            })) {
      return clonecore::Status::failure(app_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"PE MBRからGPT移行のパーティション番号",
          L"コピー元パーティション番号が重複しているため開始できません"));
    }
    ordered.push_back(&partition);
  }
  std::sort(
      ordered.begin(),
      ordered.end(),
      [](const diskmodel::PartitionInfo* left,
         const diskmodel::PartitionInfo* right) {
        return left->offset_bytes < right->offset_bytes;
      });
  for (std::size_t index = 1U; index < ordered.size(); ++index) {
    const auto* previous = ordered[index - 1U];
    const auto* current = ordered[index];
    if (previous->offset_bytes + previous->size_bytes >
        current->offset_bytes) {
      return clonecore::Status::failure(app_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE MBRからGPT移行のパーティション範囲",
          L"コピー元パーティション範囲が重複しているため開始できません"));
    }
  }
  if (active_system_count != 1U) {
    return clonecore::Status::failure(app_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE MBRからGPT移行の起動領域",
        L"一意なActive基本NTFSシステム領域を確認できません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_mbr2gpt_execution_report(
    const Mbr2GptDirectExecutionReport& report) {
  const auto clone_status = validate_clone_execution_report(
      report.clone, diskmodel::PartitionStyle::mbr);
  const bool clone_verified =
      clone_status.has_value() &&
      report.clone.target_returned_online &&
      !report.clone.target_left_offline &&
      report.clone.boot_finalization_required &&
      report.clone.boot_finalization_verified;
  const bool conversion_verified =
      report.conversion.microsoft_signature_verified &&
      report.conversion.target_reidentified_before_conversion &&
      report.conversion.validation.exit_code == 0U &&
      report.conversion.conversion.exit_code == 0U;
  const bool boot_verified =
      report.boot_repair.bcdboot.microsoft_signature_verified &&
      report.boot_repair.bcdboot.exit_code == 0U &&
      report.boot_repair.boot_store_verified &&
      (!report.boot_repair.system_partition_temporarily_mounted ||
       report.boot_repair.temporary_mount_released);
  if (!clone_verified || !conversion_verified || !boot_verified ||
      !report.source_reidentified_unchanged ||
      !report.source_left_read_only ||
      !report.target_reidentified_as_gpt ||
      !report.efi_system_partition_verified ||
      !report.microsoft_reserved_partition_verified ||
      !report.offline_windows_verified ||
      !report.temporary_windows_mount_released ||
      !report.final_layout_verified ||
      !report.final_target_left_offline) {
    return clonecore::Status::failure(app_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE MBRからGPT移行の実行結果",
        L"MBRコピー、Microsoft変換、UEFI起動再構築、最終レイアウト、またはコピー先offlineを完全確認できません"));
  }
  return clonecore::success_status();
}

std::string format_plan_text(const DirectCloneOperationPlan& plan) {
  std::ostringstream stream;
  stream << "Y-TEC Tsumugi Drive PE直接クローン確認\n"
         << "コピー元: ディスク " << plan.expected_source.disk_number
         << " / " << plan.expected_source.size_bytes << " bytes / "
         << diskmodel::to_utf8(plan.source_bus_type) << '\n'
         << "コピー先: ディスク " << plan.expected_target.disk_number
         << " / " << plan.expected_target.size_bytes << " bytes / "
         << diskmodel::to_utf8(plan.target_bus_type) << '\n'
         << "コピー元パーティション数: " << plan.source_partition_count
         << '\n'
         << "コピー先の既存パーティション数: "
         << plan.target_partition_count << '\n'
         << "コピー元の健康状態: "
         << diskmodel::to_utf8(std::wstring(
                diskmodel::disk_health_state_name(plan.source_health.state)))
         << '\n'
         << "コピー先の健康状態: "
         << diskmodel::to_utf8(std::wstring(
                diskmodel::disk_health_state_name(plan.target_health.state)))
         << '\n';
  if (diskmodel::disk_health_operation_advice(plan.source_health, true) ==
      diskmodel::DiskHealthOperationAdvice::recommend_rescue) {
    stream << "警告: コピー元は救出モードを推奨します。\n";
  }
  if (plan.source_health.temperature_warning ||
      plan.target_health.temperature_warning) {
    stream << "警告: 温度状態を確認してください。温度だけでは自動停止しません。\n";
  }
  stream << "実行確認語: OK\n"
         << "この確認ではディスクへ書き込んでいません。\n";
  return stream.str();
}

std::string format_plan_json(const DirectCloneOperationPlan& plan) {
  std::ostringstream stream;
  stream << "{\"schemaVersion\":2,\"mode\":\"direct-clone-preflight\","
            "\"sourceDisk\":"
         << plan.expected_source.disk_number << ",\"targetDisk\":"
         << plan.expected_target.disk_number << ",\"sourceBytes\":"
         << plan.expected_source.size_bytes << ",\"targetBytes\":"
         << plan.expected_target.size_bytes
         << ",\"sourcePartitionCount\":" << plan.source_partition_count
         << ",\"targetPartitionCount\":" << plan.target_partition_count
         << ",\"confirmation\":\"OK\",\"executionEnabled\":false}\n";
  return stream.str();
}

void write_failure(std::ostream& stream, const clonecore::Error& error) {
  stream << "処理を完了できませんでした: "
         << diskmodel::to_utf8(error.operation) << " (Windows error "
         << error.native_code << ") "
         << diskmodel::to_utf8(error.message) << '\n';
}

}  // namespace

Mbr2GptCanonicalDiskLayout make_mbr2gpt_canonical_disk_layout(
    const diskmodel::DiskInfo& disk) {
  Mbr2GptCanonicalDiskLayout layout{
      .disk_style = disk.partition_style,
  };
  layout.partitions.reserve(disk.partitions.size());
  for (const auto& partition : disk.partitions) {
    layout.partitions.push_back(Mbr2GptPartitionLayoutEntry{
        .number = partition.number,
        .style = partition.style,
        .type = canonical_partition_type(partition.type),
        .offset_bytes = partition.offset_bytes,
        .size_bytes = partition.size_bytes,
        .bootable = partition.bootable,
    });
  }
  std::sort(
      layout.partitions.begin(),
      layout.partitions.end(),
      [](const Mbr2GptPartitionLayoutEntry& left,
         const Mbr2GptPartitionLayoutEntry& right) {
        return std::tie(
                   left.number,
                   left.style,
                   left.type,
                   left.offset_bytes,
                   left.size_bytes,
                   left.bootable) <
            std::tie(
                   right.number,
                   right.style,
                   right.type,
                   right.offset_bytes,
                   right.size_bytes,
                   right.bootable);
      });
  return layout;
}

clonecore::Status validate_mbr2gpt_reviewed_layouts(
    const Mbr2GptCanonicalDiskLayout& expected_source,
    const Mbr2GptCanonicalDiskLayout& expected_target,
    const diskmodel::DiskInfo& observed_source,
    const diskmodel::DiskInfo& observed_target) {
  if (make_mbr2gpt_canonical_disk_layout(observed_source) !=
          expected_source ||
      make_mbr2gpt_canonical_disk_layout(observed_target) !=
          expected_target) {
    return clonecore::Status::failure(app_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"PE MBRからGPT移行のレビュー済みレイアウト照合",
        L"レビュー後にコピー元またはコピー先のパーティション構成が変わったため開始できません"));
  }
  return clonecore::success_status();
}

clonecore::Result<DirectCloneOperationPlan> prepare_direct_clone_operation(
    const std::uint32_t source_disk_number,
    const std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider) {
  auto inventory = provider.enumerate();
  if (!inventory) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(
        inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(app_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"PE直接クローンの全ディスク列挙",
        L"未解決の列挙診断があるため対象を選択できません"));
  }

  const auto source = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [source_disk_number](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == source_disk_number;
      });
  const auto target = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [target_disk_number](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == target_disk_number;
      });
  if (source == inventory.value().disks.end() ||
      target == inventory.value().disks.end()) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(app_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"PE直接クローンの対象選択",
        L"指定したコピー元またはコピー先ディスクが見つかりません"));
  }

  const auto readiness = validate_clone_execution_observation(*source, *target);
  if (!readiness) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(
        readiness.error());
  }
  auto source_identity =
      diskmodel::make_stable_disk_identity(*source, source->is_system_disk);
  if (!source_identity) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(
        source_identity.error());
  }
  auto target_identity =
      diskmodel::make_stable_disk_identity(*target, target->is_system_disk);
  if (!target_identity) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(
        target_identity.error());
  }
  const auto selection = clonecore::validate_clone_selection(
      source_identity.value(),
      source_identity.value(),
      target_identity.value(),
      target_identity.value());
  if (!selection) {
    return clonecore::Result<DirectCloneOperationPlan>::failure(
        selection.error());
  }

  return clonecore::Result<DirectCloneOperationPlan>::success(
      DirectCloneOperationPlan{
          .expected_source = source_identity.take_value(),
          .expected_target = target_identity.take_value(),
          .source_partition_style = source->partition_style,
          .source_bus_type = source->bus_type,
          .target_bus_type = target->bus_type,
          .source_partition_count = source->partitions.size(),
          .target_partition_count = target->partitions.size(),
          .source_health = source->health,
          .target_health = target->health,
      });
}

clonecore::Result<Mbr2GptDirectOperationPlan>
prepare_mbr2gpt_direct_operation(
    const std::uint32_t source_disk_number,
    const std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider) {
  auto inventory = provider.enumerate();
  if (!inventory) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(app_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"PE MBRからGPT移行の全ディスク列挙",
        L"未解決の列挙診断があるため対象を選択できません"));
  }

  const auto source = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [source_disk_number](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == source_disk_number;
      });
  const auto target = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [target_disk_number](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == target_disk_number;
      });
  if (source == inventory.value().disks.end() ||
      target == inventory.value().disks.end()) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(app_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"PE MBRからGPT移行の対象選択",
        L"指定したコピー元またはコピー先ディスクが見つかりません"));
  }

  const auto clone_ready =
      validate_clone_execution_observation(*source, *target);
  if (!clone_ready) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        clone_ready.error());
  }
  const auto mbr_ready = validate_mbr2gpt_source_layout(*source);
  if (!mbr_ready) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        mbr_ready.error());
  }

  auto source_identity =
      diskmodel::make_stable_disk_identity(*source, source->is_system_disk);
  if (!source_identity) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        source_identity.error());
  }
  auto target_identity =
      diskmodel::make_stable_disk_identity(*target, target->is_system_disk);
  if (!target_identity) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        target_identity.error());
  }
  const auto selection = clonecore::validate_clone_selection(
      source_identity.value(),
      source_identity.value(),
      target_identity.value(),
      target_identity.value());
  if (!selection) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        selection.error());
  }

  const auto active = std::find_if(
      source->partitions.begin(),
      source->partitions.end(),
      [](const diskmodel::PartitionInfo& partition) {
        return partition.bootable;
      });
  if (active == source->partitions.end()) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(app_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"PE MBRからGPT移行のレビュー計画",
        L"検証済みActive領域を不変計画へ保持できません"));
  }

  Mbr2GptDirectOperationPlan plan{
      .clone = DirectCloneOperationPlan{
          .expected_source = source_identity.take_value(),
          .expected_target = target_identity.take_value(),
          .source_partition_style = source->partition_style,
          .source_bus_type = source->bus_type,
          .target_bus_type = target->bus_type,
          .source_partition_count = source->partitions.size(),
          .target_partition_count = target->partitions.size(),
          .source_health = source->health,
          .target_health = target->health,
      },
      .primary_partition_count = source->partitions.size(),
      .active_system_partition_number = active->number,
      .source_layout = make_mbr2gpt_canonical_disk_layout(*source),
      .target_layout = make_mbr2gpt_canonical_disk_layout(*target),
  };
  auto binding = calculate_mbr2gpt_review_binding(plan);
  if (!binding) {
    return clonecore::Result<Mbr2GptDirectOperationPlan>::failure(
        binding.error());
  }
  plan.review_binding_digest = binding.take_value();
  return clonecore::Result<Mbr2GptDirectOperationPlan>::success(
      std::move(plan));
}

clonecore::Result<CloneExecutionReport> execute_direct_clone_operation(
    const DirectCloneOperationPlan& plan,
    const bool target_erasure_acknowledged,
    const std::wstring_view typed_confirmation,
    std::wstring authorization,
    ICloneExecutionService& service,
    clonecore::DiskOperationCallbacks callbacks) {
  if (!target_erasure_acknowledged || typed_confirmation != L"OK") {
    return clonecore::Result<CloneExecutionReport>::failure(app_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_ACCESS_DENIED,
        L"PE直接クローンの最終確認",
        L"コピー先の消去確認後、大文字で OK と入力してください"));
  }

  const clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = true,
      .typed_token = clonecore::make_target_confirmation_token(
          plan.expected_target),
  };
  auto execution = service.execute(CloneExecutionRequest{
      .expected_source = plan.expected_source,
      .expected_target = plan.expected_target,
      .confirmation = confirmation,
      .authorization = std::move(authorization),
      .callbacks = std::move(callbacks),
      .leave_target_offline = true,
  });
  if (!execution) {
    return execution;
  }
  const auto report_status = validate_clone_execution_report(
      execution.value(), plan.source_partition_style);
  if (!report_status) {
    return clonecore::Result<CloneExecutionReport>::failure(
        report_status.error());
  }
  if (!execution.value().target_left_offline ||
      execution.value().target_returned_online) {
    return clonecore::Result<CloneExecutionReport>::failure(app_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        L"PE直接クローンの最終ディスク状態",
        L"コピー先がofflineの換装待ち状態で確認できません"));
  }
  return execution;
}

clonecore::Result<Mbr2GptDirectExecutionReport>
execute_mbr2gpt_direct_operation(
    const Mbr2GptDirectOperationPlan& plan,
    const bool target_erasure_acknowledged,
    const std::wstring_view typed_confirmation,
    IMbr2GptDirectExecutionService& service,
    clonecore::DiskOperationCallbacks callbacks) {
  if (!target_erasure_acknowledged || typed_confirmation != L"OK") {
    return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
        app_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_ACCESS_DENIED,
            L"PE MBRからGPT移行の最終確認",
            L"コピー先の全消去と形式変換を確認後、大文字で OK と入力してください"));
  }
  auto reviewed_binding = calculate_mbr2gpt_review_binding(plan);
  if (!reviewed_binding) {
    return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
        reviewed_binding.error());
  }
  if (plan.clone.source_partition_style !=
          diskmodel::PartitionStyle::mbr ||
      plan.primary_partition_count == 0U ||
      plan.primary_partition_count > 3U ||
      plan.clone.source_partition_count != plan.primary_partition_count ||
      plan.source_layout.disk_style != diskmodel::PartitionStyle::mbr ||
      plan.source_layout.partitions.size() !=
          plan.primary_partition_count ||
      plan.target_layout.partitions.size() !=
          plan.clone.target_partition_count ||
      plan.active_system_partition_number == 0U ||
      plan.active_system_partition_number > 4U ||
      reviewed_binding.value() != plan.review_binding_digest) {
    return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
        app_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"PE MBRからGPT移行のレビュー計画",
            L"レビュー済みMBR構成が不正または変更されているため実行できません"));
  }

  const clonecore::TargetConfirmation confirmation{
      .first_step_acknowledged = true,
      .typed_token = clonecore::make_target_confirmation_token(
          plan.clone.expected_target),
  };
  auto execution = service.execute(Mbr2GptDirectExecutionRequest{
      .expected_source = plan.clone.expected_source,
      .expected_target = plan.clone.expected_target,
      .expected_source_layout = plan.source_layout,
      .expected_target_layout = plan.target_layout,
      .confirmation = confirmation,
      .callbacks = std::move(callbacks),
  });
  if (!execution) {
    return execution;
  }
  const auto report_status =
      validate_mbr2gpt_execution_report(execution.value());
  if (!report_status) {
    return clonecore::Result<Mbr2GptDirectExecutionReport>::failure(
        report_status.error());
  }
  return execution;
}

int run_winpe_app(
    const std::vector<std::wstring>& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    std::ostream& output,
    std::ostream& error_output) {
  if (arguments.size() == 1U && is_help_argument(arguments.front())) {
    write_usage(output);
    return static_cast<int>(clitools::CliExitCode::success);
  }
  if (arguments.empty() ||
      (arguments.size() == 1U &&
       (arguments.front() == L"--text" || arguments.front() == L"--json"))) {
    return clitools::run_inventory_cli(
        arguments, provider, output, error_output, kWinPePresentation);
  }

  const auto direct = parse_direct_arguments(arguments);
  if (!direct.has_value()) {
    error_output << "引数が不正です。\n";
    write_usage(error_output);
    return static_cast<int>(clitools::CliExitCode::invalid_arguments);
  }

  auto plan = prepare_direct_clone_operation(
      direct->source_disk_number, direct->target_disk_number, provider);
  if (!plan) {
    write_failure(error_output, plan.error());
    return static_cast<int>(clitools::CliExitCode::failure);
  }
  output << (direct->format == OutputFormat::json
                 ? format_plan_json(plan.value())
                 : format_plan_text(plan.value()));
  return static_cast<int>(clitools::CliExitCode::success);
}

}  // namespace ytec::winpeapp

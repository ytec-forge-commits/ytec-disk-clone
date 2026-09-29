#include "ytec/winpeapp/rescue_clone.h"

#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/winpeapp/active_rescue_media.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

clonecore::Error rescue_product_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(rescue_product_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool all_zero(const imageformat::Sha256Digest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_bool(std::vector<std::byte>& bytes, const bool value) {
  append_u8(bytes, value ? std::uint8_t{1} : std::uint8_t{0});
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_domain(
    std::vector<std::byte>& bytes,
    const std::string_view domain) {
  append_u32(bytes, static_cast<std::uint32_t>(domain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
}

clonecore::Status validate_rescue_observation(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    const bool target_is_active_rescue_media) {
  if (!source.offline.has_value() || !source.read_only.has_value() ||
      !source.removable.has_value() || !target.offline.has_value() ||
      !target.read_only.has_value() || !target.removable.has_value()) {
    return clonecore::Status::failure(rescue_product_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"PE救出クローンのディスク属性",
        L"コピー元・コピー先のoffline、read-only、removable属性を確定できません"));
  }
  if (source.size_bytes == 0U || target.size_bytes < source.size_bytes ||
      source.logical_sector_size != 512U ||
      target.logical_sector_size != source.logical_sector_size) {
    return clonecore::Status::failure(rescue_product_error(
        clonecore::ErrorCode::unsupported_layout,
        target.size_bytes < source.size_bytes ? ERROR_DISK_FULL
                                               : ERROR_NOT_SUPPORTED,
        L"PE救出RAWコピーの寸法",
        L"同じ512バイト論理セクターで、コピー先がコピー元と同容量以上の場合だけ救出できます"));
  }
  if (source.removable.value() || source.bus_type.empty() ||
      target.bus_type.empty()) {
    return clonecore::Status::failure(rescue_product_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE救出コピー元の安全分類",
        L"接続方式を確定できる固定ディスクだけを救出コピー元にできます"));
  }
  const auto source_class =
      imageformat::classify_tsumugi_physical_restore_target(source);
  if (source_class.usb_memory || source_class.dynamic_disk ||
      source_class.storage_spaces || source_class.software_raid ||
      source_class.unresolved_hardware_raid ||
      source_class.unsupported_virtual) {
    return clonecore::Status::failure(rescue_product_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE救出コピー元の構成",
        L"USBメモリ、Dynamic Disk、Storage Spaces、RAID、仮想ディスクは救出コピー元にできません"));
  }
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(target);
  const auto target_valid = imageformat::validate_tsumugi_physical_restore_target(
      target, target_class, target_is_active_rescue_media);
  if (!target_valid) {
    return target_valid;
  }
  return clonecore::success_status();
}

clonecore::Result<operationcore::OperationId> make_operation_id() {
  GUID guid{};
  const HRESULT created = CoCreateGuid(&guid);
  if (FAILED(created)) {
    return failure<operationcore::OperationId>(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(created),
        L"PE救出クローン操作ID",
        L"単回操作IDを生成できません");
  }
  operationcore::OperationId id{};
  static_assert(sizeof(guid) == id.size());
  std::memcpy(id.data(), &guid, id.size());
  return clonecore::Result<operationcore::OperationId>::success(id);
}

clonecore::Result<operationcore::Sha256Digest> immutable_payload_hash(
    const RescueCloneOperationPlan& reviewed) {
  std::vector<std::byte> bytes;
  bytes.reserve(128U);
  append_domain(bytes, "YTEC-WINPE-RESCUE-CLONE-PLAN-V1");
  append_array(bytes, reviewed.expected_source_layout_hash);
  append_array(bytes, reviewed.expected_target_layout_hash);
  append_u64(bytes, reviewed.expected_source.size_bytes);
  append_u64(bytes, reviewed.expected_target.size_bytes);
  append_u64(bytes, reviewed.large_block_bytes);
  append_u8(
      bytes,
      static_cast<std::uint8_t>(reviewed.source_partition_style));
  append_bool(bytes, reviewed.active_rescue_media_checked);
  // These immutable zeros explicitly bind the plan to no shrink, no
  // conversion, and no boot-finalization behavior.
  append_u8(bytes, std::uint8_t{0});
  append_u8(bytes, std::uint8_t{0});
  append_u8(bytes, std::uint8_t{0});
  return imageformat::sha256(bytes);
}

clonecore::Result<operationcore::Sha256Digest> execution_evidence_hash(
    const operationcore::OperationPlan& plan,
    const RescueCloneExecutionReport& report) {
  std::vector<std::byte> bytes;
  bytes.reserve(256U + report.raw.missing_ranges.size() * 80U);
  append_domain(bytes, "YTEC-WINPE-RESCUE-CLONE-EVIDENCE-V1");
  append_array(bytes, plan.immutable_payload_hash);
  append_u64(bytes, report.raw.source_extent_bytes);
  append_u64(bytes, report.raw.copied_source_bytes);
  append_u64(bytes, report.raw.recovered_bytes);
  append_u64(bytes, report.raw.zero_filled_bytes);
  append_u64(bytes, report.raw.written_and_read_back_verified_bytes);
  append_u64(bytes, report.untouched_target_tail_bytes);
  append_u64(bytes, report.raw.forward_failed_block_count);
  append_u64(bytes, report.raw.reverse_recovered_block_count);
  append_u64(bytes, report.raw.reverse_failed_block_count);
  append_u64(bytes, report.raw.sector_recovered_count);
  append_u64(bytes, report.raw.exhausted_sector_count);
  append_bool(bytes, report.raw.layout_preserved_without_conversion);
  append_bool(bytes, report.raw.byte_exact_copy);
  append_bool(bytes, report.raw.target_flushed);
  append_bool(bytes, report.raw.all_writes_read_back_verified);
  append_bool(bytes, report.raw.partial_data_loss);
  append_bool(bytes, report.source_left_read_only);
  append_bool(bytes, report.target_left_offline);
  append_bool(bytes, report.active_rescue_media_excluded_by_stable_identity);
  append_bool(bytes, report.must_display_as_partial_loss);
  append_bool(bytes, report.shrinking_performed);
  append_bool(bytes, report.partition_style_conversion_performed);
  append_bool(bytes, report.boot_finalization_performed);
  append_u32(
      bytes,
      static_cast<std::uint32_t>(report.raw.missing_ranges.size()));
  for (const auto& missing : report.raw.missing_ranges) {
    append_u64(bytes, missing.bytes.offset);
    append_u64(bytes, missing.bytes.length);
    append_u64(bytes, missing.first_lba);
    append_u64(bytes, missing.sector_count);
    append_u8(bytes, missing.forward_attempts);
    append_u8(bytes, missing.reverse_attempts);
    append_u8(bytes, missing.sector_attempts);
    append_u32(bytes, missing.forward_native_error);
    append_u32(bytes, missing.reverse_native_error);
    append_u32(bytes, missing.sector_native_error);
    append_bool(bytes, missing.zero_fill_read_back_verified);
  }
  return imageformat::sha256(bytes);
}

clonecore::Status validate_execution_report(
    const operationcore::OperationPlan& plan,
    const RescueCloneExecutionReport& report) {
  const auto& raw = report.raw;
  if (raw.source_extent_bytes != plan.expected_work_bytes ||
      raw.written_and_read_back_verified_bytes != plan.expected_work_bytes ||
      raw.copied_source_bytes > plan.expected_work_bytes ||
      raw.zero_filled_bytes >
          plan.expected_work_bytes - raw.copied_source_bytes ||
      raw.copied_source_bytes + raw.zero_filled_bytes !=
          plan.expected_work_bytes ||
      !plan.target.has_value() ||
      plan.target->size_bytes < plan.expected_work_bytes ||
      report.untouched_target_tail_bytes !=
          plan.target->size_bytes - plan.expected_work_bytes ||
      !raw.layout_preserved_without_conversion || !raw.target_flushed ||
      !raw.all_writes_read_back_verified ||
      (raw.partial_data_loss != !raw.missing_ranges.empty()) ||
      (raw.byte_exact_copy == raw.partial_data_loss) ||
      !report.source_left_read_only || !report.target_left_offline ||
      !report.active_rescue_media_excluded_by_stable_identity ||
      !report.must_display_as_partial_loss || report.shrinking_performed ||
      report.partition_style_conversion_performed ||
      report.boot_finalization_performed) {
    return clonecore::Status::failure(rescue_product_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE救出クローンOperation最終検証",
        L"全書込み読戻し、欠損マップ、未処理末尾、コピー元read-only、コピー先offline、または救出専用結果分類の証跡が不足しています"));
  }
  return clonecore::success_status();
}

clonecore::Error append_offline_failure(
    clonecore::Error primary,
    const clonecore::Status& protected_offline) {
  if (!protected_offline) {
    primary.message +=
        L"。コピー先offline状態の再確認にも失敗しました: " +
        protected_offline.error().operation;
  }
  return primary;
}

clonecore::DiskOperationProgress translate_progress(
    const clonecore::RescueCopyProgress& progress) {
  clonecore::DiskOperationStage stage =
      clonecore::DiskOperationStage::copying_data;
  if (progress.phase == clonecore::RescueCopyPhase::validating) {
    stage = clonecore::DiskOperationStage::verifying_source;
  } else if (progress.phase == clonecore::RescueCopyPhase::flushing) {
    stage = clonecore::DiskOperationStage::flushing_data;
  } else if (progress.phase == clonecore::RescueCopyPhase::completed) {
    stage = clonecore::DiskOperationStage::completed;
  }
  return clonecore::DiskOperationProgress{
      .stage = stage,
      .partition_index = std::nullopt,
      .total_read_bytes = progress.source_extent_bytes,
      .total_write_bytes = progress.source_extent_bytes,
      .total_verify_bytes = progress.source_extent_bytes,
      .read_bytes = progress.settled_target_bytes,
      .written_bytes = progress.settled_target_bytes,
      .verified_bytes = progress.settled_target_bytes,
      .cancellation_allowed = progress.cancellation_allowed,
      .pause_allowed = progress.pause_allowed,
  };
}

clonecore::Result<RescueCloneExecutionReport> execute_rescue_engine(
    const RescueCloneOperationPlan& reviewed,
    const clonecore::TargetConfirmation& confirmation,
    const RescueCloneDependencies& dependencies,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto fail_missing_dependency = []() {
    return failure<RescueCloneExecutionReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE救出クローン依存",
        L"再識別、起動媒体除外、read-only、offline、またはI/O依存が不足しています");
  };
  if (!dependencies.reidentify_selection ||
      !dependencies.is_active_rescue_media ||
      !dependencies.set_source_read_only ||
      !dependencies.open_read_only_source ||
      !dependencies.set_target_offline ||
      !dependencies.open_offline_target) {
    return fail_missing_dependency();
  }

  auto observed = dependencies.reidentify_selection(
      reviewed.expected_source, reviewed.expected_target);
  if (!observed) {
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        observed.error());
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!source_layout || !target_layout) {
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  if (source_layout.value() != reviewed.expected_source_layout_hash ||
      target_layout.value() != reviewed.expected_target_layout_hash) {
    return failure<RescueCloneExecutionReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE救出クローン実行直前レイアウト",
        L"最終確認後にコピー元またはコピー先のレイアウトが変化しました");
  }
  auto active =
      dependencies.is_active_rescue_media(observed.value().target_identity);
  if (!active) {
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        active.error());
  }
  const auto ready = validate_rescue_observation(
      observed.value().source, observed.value().target, active.value());
  if (!ready) {
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        ready.error());
  }

  // PE rescue intentionally leaves this non-persistent disk attribute set.
  // A damaged source must not be made writable again by cleanup code.
  const auto protected_source = dependencies.set_source_read_only(
      reviewed.expected_source, true);
  if (!protected_source) {
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        protected_source.error());
  }

  auto source =
      dependencies.open_read_only_source(reviewed.expected_source);
  if (!source) {
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        source.error());
  }
  if (!source.value().observed.observed.read_only.value_or(false) ||
      source.value().observed.identity.is_system_disk !=
          reviewed.expected_source.is_system_disk) {
    return failure<RescueCloneExecutionReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_WRITE_PROTECT,
        L"PE救出コピー元read-only再確認",
        L"オープンしたコピー元をread-onlyとして再確認できません");
  }

  auto offline = dependencies.set_target_offline(
      reviewed.expected_source,
      reviewed.expected_target,
      confirmation,
      true);
  if (!offline) {
    const auto protected_offline = dependencies.set_target_offline(
        reviewed.expected_source,
        reviewed.expected_target,
        confirmation,
        true);
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        append_offline_failure(offline.error(), protected_offline));
  }

  std::optional<diskmodel::PhysicalTargetHandle> target;
  auto fail_after_target_transition = [&](clonecore::Error error) {
    if (target.has_value()) {
      target->target.reset();
    }
    source.value().reader.reset();
    const auto protected_offline = dependencies.set_target_offline(
        reviewed.expected_source,
        reviewed.expected_target,
        confirmation,
        true);
    return clonecore::Result<RescueCloneExecutionReport>::failure(
        append_offline_failure(std::move(error), protected_offline));
  };

  auto opened_target = dependencies.open_offline_target(
      reviewed.expected_target, confirmation);
  if (!opened_target) {
    return fail_after_target_transition(opened_target.error());
  }
  target.emplace(opened_target.take_value());
  const auto identities = clonecore::validate_clone_identities(
      reviewed.expected_source,
      source.value().observed.identity,
      reviewed.expected_target,
      target->observed.target_identity,
      confirmation);
  if (!identities) {
    return fail_after_target_transition(identities.error());
  }
  if (!target->observed.target.offline.value_or(false)) {
    return fail_after_target_transition(rescue_product_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"PE救出コピー先offline再確認",
        L"オープンしたコピー先がofflineではありません"));
  }

  clonecore::RescueCopyCallbacks rescue_callbacks{
      .cancellation_requested = callbacks.cancellation_requested,
      .safe_boundary = callbacks.safe_boundary,
  };
  if (callbacks.progress) {
    rescue_callbacks.progress =
        [&callbacks](const clonecore::RescueCopyProgress& progress) {
          clonecore::report_disk_operation_progress(
              callbacks, translate_progress(progress));
        };
  }
  auto rescued = clonecore::execute_rescue_raw_copy(
      clonecore::RescueRawCopyRequest{
          .environment = clonecore::RescueExecutionEnvironment::winpe,
          .source_kind = reviewed.expected_source.is_system_disk
              ? clonecore::RescueSourceKind::system_disk
              : clonecore::RescueSourceKind::data_disk,
          .rescue_mode_explicitly_confirmed = true,
          .large_block_bytes = reviewed.large_block_bytes,
          .callbacks = std::move(rescue_callbacks),
      },
      *source.value().reader,
      *target->target);
  if (!rescued) {
    return fail_after_target_transition(rescued.error());
  }

  target->target.reset();
  target.reset();
  source.value().reader.reset();
  const auto final_offline = dependencies.set_target_offline(
      reviewed.expected_source,
      reviewed.expected_target,
      confirmation,
      true);
  if (!final_offline) {
    return fail_after_target_transition(final_offline.error());
  }
  auto final_observed = dependencies.reidentify_selection(
      reviewed.expected_source, reviewed.expected_target);
  if (!final_observed) {
    return fail_after_target_transition(final_observed.error());
  }
  auto final_source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          final_observed.value().source);
  if (!final_source_layout ||
      final_source_layout.value() != reviewed.expected_source_layout_hash ||
      !final_observed.value().source.read_only.value_or(false) ||
      !final_observed.value().target.offline.value_or(false)) {
    return fail_after_target_transition(
        !final_source_layout
            ? final_source_layout.error()
            : rescue_product_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"PE救出クローン最終ディスク状態",
                  L"コピー元レイアウト/read-onlyまたはコピー先offlineを最終確認できません"));
  }

  RescueCloneExecutionReport report{
      .raw = rescued.take_value(),
      .untouched_target_tail_bytes =
          reviewed.expected_target.size_bytes -
          reviewed.expected_source.size_bytes,
      .source_left_read_only = true,
      .target_left_offline = true,
      .active_rescue_media_excluded_by_stable_identity = true,
      .must_display_as_partial_loss = true,
      .shrinking_performed = false,
      .partition_style_conversion_performed = false,
      .boot_finalization_performed = false,
  };
  const auto valid_report = validate_execution_report(
      operationcore::OperationPlan{
          .operation_id = reviewed.operation_id,
          .kind = operationcore::OperationKind::rescue_clone,
          .environment = operationcore::OperationEnvironment::winpe,
          .source = reviewed.expected_source,
          .target = reviewed.expected_target,
          .expected_work_bytes = reviewed.expected_source.size_bytes,
          .immutable_payload_hash = reviewed.expected_source_layout_hash,
      },
      report);
  if (!valid_report) {
    return fail_after_target_transition(valid_report.error());
  }
  return clonecore::Result<RescueCloneExecutionReport>::success(
      std::move(report));
}

clonecore::Result<operationcore::ReidentifiedOperation>
reidentify_for_operation(
    const RescueCloneOperationPlan& reviewed,
    const RescueCloneDependencies& dependencies) {
  if (!dependencies.reidentify_selection ||
      !dependencies.is_active_rescue_media) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE救出Operation再識別依存",
        L"再識別または起動媒体除外依存がありません");
  }
  auto observed = dependencies.reidentify_selection(
      reviewed.expected_source, reviewed.expected_target);
  if (!observed) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        observed.error());
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!source_layout || !target_layout) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  auto active =
      dependencies.is_active_rescue_media(observed.value().target_identity);
  if (!active) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        active.error());
  }
  const auto ready = validate_rescue_observation(
      observed.value().source, observed.value().target, active.value());
  if (!ready) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        ready.error());
  }
  if (source_layout.value() != reviewed.expected_source_layout_hash ||
      target_layout.value() != reviewed.expected_target_layout_hash) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE救出Operationレイアウト再識別",
        L"画面で確認したコピー元またはコピー先のレイアウトが変化しました");
  }
  return clonecore::Result<operationcore::ReidentifiedOperation>::success({
      .source = observed.value().source_identity,
      .target = observed.value().target_identity,
  });
}

}  // namespace

std::wstring format_rescue_clone_product_result(
    const RescueCloneExecutionReport& report) {
  const auto& raw = report.raw;
  std::wostringstream stream;
  stream
      << kRescueCloneResultClassification
      << L"\r\nこれは通常クローン成功・起動成功の表示ではありません。\r\n\r\n"
         L"対象範囲: "
      << raw.source_extent_bytes << L" bytes"
      << L"\r\n読取回復済み: " << raw.copied_source_bytes << L" bytes"
      << L"\r\n再試行で回復: " << raw.recovered_bytes << L" bytes"
      << L"\r\nゼロ埋め: " << raw.zero_filled_bytes << L" bytes"
      << L"\r\n全書込み読戻し: "
      << (raw.all_writes_read_back_verified ? L"合格" : L"未完了")
      << L"\r\nコピー先flush: "
      << (raw.target_flushed ? L"完了" : L"未完了")
      << L"\r\nコピー先の未処理末尾: "
      << report.untouched_target_tail_bytes
      << L" bytes（コピー元容量を超える範囲は消去・検証していません）"
      << L"\r\nコピー元read-only保持: "
      << (report.source_left_read_only ? L"確認済み" : L"未確認")
      << L"\r\nコピー先offline保持: "
      << (report.target_left_offline ? L"確認済み" : L"未確認")
      << L"\r\n\r\n実欠損map: ";
  if (!raw.partial_data_loss) {
    stream
        << L"0件（全範囲を回復）\r\n"
           L"ただし救出処理のため、結果分類は「一部欠損の可能性あり」のままです。";
    return stream.str();
  }

  stream << raw.missing_ranges.size() << L"件 / "
         << raw.zero_filled_bytes << L" bytes\r\n";
  constexpr std::size_t kDisplayedMissingRangeLimit = 32U;
  const std::size_t displayed =
      (std::min)(raw.missing_ranges.size(), kDisplayedMissingRangeLimit);
  for (std::size_t index = 0; index < displayed; ++index) {
    const auto& missing = raw.missing_ranges[index];
    stream << L"  [" << index + 1U << L"] offset "
           << missing.bytes.offset << L" / length "
           << missing.bytes.length << L" / LBA "
           << missing.first_lba << L" / sectors "
           << missing.sector_count << L" / zero-fill読戻し "
           << (missing.zero_fill_read_back_verified ? L"合格" : L"未完了")
           << L"\r\n";
  }
  if (displayed < raw.missing_ranges.size()) {
    stream << L"  ...ほか " << raw.missing_ranges.size() - displayed
           << L"件（画面の安全な表示上限により省略）\r\n";
  }
  return stream.str();
}

clonecore::Result<RescueCloneOperationPlan>
prepare_rescue_clone_operation(
    const std::uint32_t source_disk_number,
    const std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider,
    const RescueCloneActiveMediaQuery& active_rescue_media_query) {
  if (!active_rescue_media_query) {
    return failure<RescueCloneOperationPlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE救出クローン起動媒体除外",
        L"現在このPEを起動しているレスキュー媒体の確認機能がありません");
  }
  auto inventory = provider.enumerate();
  if (!inventory) {
    return clonecore::Result<RescueCloneOperationPlan>::failure(
        inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return failure<RescueCloneOperationPlan>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"PE救出クローンの全ディスク列挙",
        L"未解決の列挙診断があるため対象を選択できません");
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
      target == inventory.value().disks.end() || source == target) {
    return failure<RescueCloneOperationPlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"PE救出クローンの対象選択",
        L"別々のコピー元とコピー先ディスクを選択してください");
  }
  auto source_identity = diskmodel::make_stable_disk_identity(
      *source, source->is_system_disk);
  auto target_identity = diskmodel::make_stable_disk_identity(
      *target, target->is_system_disk);
  if (!source_identity || !target_identity) {
    return clonecore::Result<RescueCloneOperationPlan>::failure(
        !source_identity ? source_identity.error() : target_identity.error());
  }
  const auto identities = clonecore::validate_clone_selection(
      source_identity.value(),
      source_identity.value(),
      target_identity.value(),
      target_identity.value());
  if (!identities) {
    return clonecore::Result<RescueCloneOperationPlan>::failure(
        identities.error());
  }
  auto active = active_rescue_media_query(target_identity.value());
  if (!active) {
    return clonecore::Result<RescueCloneOperationPlan>::failure(
        active.error());
  }
  const auto ready =
      validate_rescue_observation(*source, *target, active.value());
  if (!ready) {
    return clonecore::Result<RescueCloneOperationPlan>::failure(
        ready.error());
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(*source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(*target);
  auto operation_id = make_operation_id();
  if (!source_layout || !target_layout || !operation_id) {
    if (!source_layout) {
      return clonecore::Result<RescueCloneOperationPlan>::failure(
          source_layout.error());
    }
    if (!target_layout) {
      return clonecore::Result<RescueCloneOperationPlan>::failure(
          target_layout.error());
    }
    return clonecore::Result<RescueCloneOperationPlan>::failure(
        operation_id.error());
  }
  return clonecore::Result<RescueCloneOperationPlan>::success({
      .operation_id = operation_id.take_value(),
      .expected_source = source_identity.take_value(),
      .expected_target = target_identity.take_value(),
      .expected_source_layout_hash = source_layout.take_value(),
      .expected_target_layout_hash = target_layout.take_value(),
      .source_partition_style = source->partition_style,
      .source_bus_type = source->bus_type,
      .target_bus_type = target->bus_type,
      .source_partition_count = source->partitions.size(),
      .target_partition_count = target->partitions.size(),
      .source_health = source->health,
      .target_health = target->health,
      .large_block_bytes = 4U * 1024U * 1024U,
      .active_rescue_media_checked = true,
  });
}

clonecore::Result<RescueCloneOperationReport>
execute_rescue_clone_operation(
    const RescueCloneOperationPlan& reviewed_plan,
    const bool target_erasure_acknowledged,
    const std::wstring_view typed_confirmation,
    const RescueCloneDependencies& dependencies,
    clonecore::DiskOperationCallbacks callbacks) {
  if (!target_erasure_acknowledged) {
    return failure<RescueCloneOperationReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"PE救出クローンの消去確認",
        L"コピー先ディスク全体の消去内容を確認してください");
  }
  if (!reviewed_plan.active_rescue_media_checked ||
      reviewed_plan.expected_source.logical_sector_size == 0U ||
      reviewed_plan.large_block_bytes == 0U ||
      reviewed_plan.large_block_bytes > 16U * 1024U * 1024U ||
      reviewed_plan.large_block_bytes %
              reviewed_plan.expected_source.logical_sector_size !=
          0U ||
      all_zero(reviewed_plan.expected_source_layout_hash) ||
      all_zero(reviewed_plan.expected_target_layout_hash)) {
    return failure<RescueCloneOperationReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE救出クローンレビュー",
        L"起動媒体除外、レイアウトHash、または救出ブロック設定が不正です");
  }
  auto payload_hash = immutable_payload_hash(reviewed_plan);
  if (!payload_hash) {
    return clonecore::Result<RescueCloneOperationReport>::failure(
        payload_hash.error());
  }
  operationcore::OperationPlan plan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = reviewed_plan.operation_id,
      .kind = operationcore::OperationKind::rescue_clone,
      .environment = operationcore::OperationEnvironment::winpe,
      .source = reviewed_plan.expected_source,
      .target = reviewed_plan.expected_target,
      .expected_work_bytes = reviewed_plan.expected_source.size_bytes,
      .immutable_payload_hash = payload_hash.take_value(),
  };
  const auto plan_valid = operationcore::validate_operation_plan(plan);
  if (!plan_valid) {
    return clonecore::Result<RescueCloneOperationReport>::failure(
        plan_valid.error());
  }

  const clonecore::TargetConfirmation target_confirmation{
      .first_step_acknowledged = true,
      .typed_token = L"OK",
  };
  std::optional<RescueCloneExecutionReport> rescue;
  operationcore::OperationCallbacks operation_callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan&) {
            return reidentify_for_operation(reviewed_plan, dependencies);
          },
      .execute =
          [&](const operationcore::OperationPlan& current,
              const clonecore::DiskOperationCallbacks& operation_progress) {
            auto executed = execute_rescue_engine(
                reviewed_plan,
                target_confirmation,
                dependencies,
                operation_progress);
            if (!executed) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  executed.error());
            }
            rescue = executed.take_value();
            const auto valid_report = validate_execution_report(
                current, rescue.value());
            if (!valid_report) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  valid_report.error());
            }
            auto evidence = execution_evidence_hash(current, rescue.value());
            if (!evidence) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  evidence.error());
            }
            return clonecore::Result<
                operationcore::ExecutionEvidence>::success({
                .processed_work_bytes = current.expected_work_bytes,
                .output_hash = evidence.take_value(),
            });
          },
      .verify =
          [&](const operationcore::OperationPlan& current,
              const operationcore::ExecutionEvidence& execution,
              const clonecore::DiskOperationCallbacks&) {
            if (!rescue.has_value()) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"PE救出クローンOperation証跡",
                  L"救出実行結果がありません");
            }
            const auto valid_report = validate_execution_report(
                current, rescue.value());
            if (!valid_report) {
              return clonecore::Result<
                  operationcore::VerificationEvidence>::failure(
                  valid_report.error());
            }
            auto evidence = execution_evidence_hash(current, rescue.value());
            if (!evidence) {
              return clonecore::Result<
                  operationcore::VerificationEvidence>::failure(
                  evidence.error());
            }
            if (execution.output_hash != evidence.value()) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"PE救出クローンOperation Hash照合",
                  L"実行時と最終検証時の救出証跡が一致しません");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = current.expected_work_bytes,
                .output_hash = evidence.take_value(),
            });
          },
      .disk_operation = std::move(callbacks),
  };
  auto lifecycle = operationcore::run_operation(
      plan, typed_confirmation, operation_callbacks);
  return clonecore::Result<RescueCloneOperationReport>::success({
      .plan = std::move(plan),
      .lifecycle = std::move(lifecycle),
      .rescue = std::move(rescue),
  });
}

RescueCloneDependencies make_rescue_clone_windows_dependencies() {
  return RescueCloneDependencies{
      .reidentify_selection =
          [](const clonecore::StableDiskIdentity& source,
             const clonecore::StableDiskIdentity& target) {
            auto inventory = diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone_selection(
                source, target, *inventory, true);
          },
      .is_active_rescue_media =
          [](const clonecore::StableDiskIdentity& target) {
            return query_active_rescue_media_target_with_windows_apis(
                target, {});
          },
      .set_source_read_only =
          diskmodel::set_verified_source_read_only_with_windows_apis,
      .open_read_only_source =
          diskmodel::open_verified_read_only_physical_disk_with_windows_apis,
      .set_target_offline =
          diskmodel::set_verified_target_offline_with_windows_apis,
      .open_offline_target =
          diskmodel::open_verified_physical_target_with_windows_apis,
  };
}

}  // namespace ytec::winpeapp

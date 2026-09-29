#include "ytec/winpeapp/rescue_clone.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ReadKey = std::pair<std::uint64_t, std::size_t>;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error injected_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = operation,
      .message = L"test failure injection",
  };
}

ytec::diskmodel::DiskInfo make_source_disk(const bool system_disk = true) {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 0U;
  disk.device_path = L"\\\\.\\PhysicalDrive0";
  disk.device_instance_id = L"SYNTHETIC\\RESCUE\\SOURCE";
  disk.model = L"RESCUE SOURCE";
  disk.size_bytes = 4096U;
  disk.sector_count = 8U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 512U;
  disk.bus_type = L"SATA";
  disk.serial_suffix = "SOURCE01";
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.is_system_disk = system_disk;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1U,
      .offset_bytes = 512U,
      .size_bytes = 3072U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
      .name = L"Data",
  });
  return disk;
}

ytec::diskmodel::DiskInfo make_target_disk() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 1U;
  disk.device_path = L"\\\\.\\PhysicalDrive1";
  disk.device_instance_id = L"SYNTHETIC\\RESCUE\\TARGET";
  disk.model = L"RESCUE TARGET";
  disk.size_bytes = 8192U;
  disk.sector_count = 16U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 512U;
  disk.bus_type = L"SATA";
  disk.serial_suffix = "TARGET01";
  disk.partition_style = ytec::diskmodel::PartitionStyle::raw;
  disk.is_system_disk = false;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  return disk;
}

class StaticInventory final
    : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  explicit StaticInventory(ytec::diskmodel::InventoryReport report)
      : report_(std::move(report)) {}

  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(report_);
  }

  std::size_t calls{};

 private:
  ytec::diskmodel::InventoryReport report_;
};

struct RescueFixture;

class FixtureSource final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit FixtureSource(RescueFixture* fixture) : fixture_(fixture) {}

  std::uint64_t size_bytes() const noexcept override;
  std::uint32_t logical_sector_size() const noexcept override;
  ytec::clonecore::Result<std::vector<std::byte>> read(
      std::uint64_t offset,
      std::size_t length) const override;

 private:
  RescueFixture* fixture_{};
};

class FixtureTarget final : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit FixtureTarget(RescueFixture* fixture) : fixture_(fixture) {}

  std::uint64_t size_bytes() const noexcept override;
  std::uint32_t logical_sector_size() const noexcept override;
  ytec::clonecore::Status write_target(
      std::uint64_t offset,
      std::span<const std::byte> bytes) override;
  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      std::uint64_t offset,
      std::size_t length) const override;
  ytec::clonecore::Status flush_target() override;

 private:
  RescueFixture* fixture_{};
};

struct RescueFixture final {
  ytec::diskmodel::DiskInfo source = make_source_disk();
  ytec::diskmodel::DiskInfo target = make_target_disk();
  std::vector<std::byte> source_bytes;
  std::vector<std::byte> target_bytes;
  std::map<ReadKey, std::size_t> recoverable_read_failures;
  std::vector<std::wstring> calls;
  std::size_t source_read_count{};
  std::size_t target_write_count{};
  std::size_t target_read_back_count{};
  std::size_t target_flush_count{};
  bool active_rescue_media{};
  bool corrupt_target_read_back{};

  RescueFixture()
      : source_bytes(static_cast<std::size_t>(source.size_bytes)),
        target_bytes(
            static_cast<std::size_t>(target.size_bytes),
            std::byte{0x7F}) {
    for (std::size_t index = 0U; index < source_bytes.size(); ++index) {
      source_bytes[index] =
          std::byte{static_cast<unsigned char>((index % 251U) + 1U)};
    }
  }

  ytec::clonecore::Result<
      ytec::diskmodel::ReidentifiedPhysicalClone>
  reidentify(
      const ytec::clonecore::StableDiskIdentity& expected_source,
      const ytec::clonecore::StableDiskIdentity& expected_target) {
    calls.push_back(L"reidentify");
    auto source_identity = ytec::diskmodel::make_stable_disk_identity(
        source, source.is_system_disk);
    auto target_identity = ytec::diskmodel::make_stable_disk_identity(
        target, target.is_system_disk);
    if (!source_identity || !target_identity) {
      return ytec::clonecore::Result<
          ytec::diskmodel::ReidentifiedPhysicalClone>::failure(
          !source_identity ? source_identity.error()
                           : target_identity.error());
    }
    const auto valid = ytec::clonecore::validate_clone_selection(
        expected_source,
        source_identity.value(),
        expected_target,
        target_identity.value());
    if (!valid) {
      return ytec::clonecore::Result<
          ytec::diskmodel::ReidentifiedPhysicalClone>::failure(
          valid.error());
    }
    return ytec::clonecore::Result<
        ytec::diskmodel::ReidentifiedPhysicalClone>::success({
        .source = source,
        .target = target,
        .source_identity = source_identity.take_value(),
        .target_identity = target_identity.take_value(),
    });
  }

  ytec::winpeapp::RescueCloneDependencies dependencies() {
    return ytec::winpeapp::RescueCloneDependencies{
        .reidentify_selection =
            [this](const auto& source_identity,
                   const auto& target_identity) {
              return reidentify(source_identity, target_identity);
            },
        .is_active_rescue_media =
            [this](const auto&) {
              calls.push_back(L"active-media");
              return ytec::clonecore::Result<bool>::success(
                  active_rescue_media);
            },
        .set_source_read_only =
            [this](const auto&, const bool read_only) {
              calls.push_back(L"source-read-only");
              source.read_only = read_only;
              return ytec::clonecore::success_status();
            },
        .open_read_only_source =
            [this](const auto&) {
              calls.push_back(L"open-source");
              auto identity = ytec::diskmodel::make_stable_disk_identity(
                  source, source.is_system_disk);
              if (!identity) {
                return ytec::clonecore::Result<
                    ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::failure(
                    identity.error());
              }
              return ytec::clonecore::Result<
                  ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success({
                  .observed = {
                      .observed = source,
                      .identity = identity.take_value(),
                  },
                  .reader = std::make_unique<FixtureSource>(this),
              });
            },
        .set_target_offline =
            [this](const auto&, const auto&, const auto&, const bool offline) {
              calls.push_back(L"target-offline");
              target.offline = offline;
              return ytec::clonecore::success_status();
            },
        .open_offline_target =
            [this](const auto&, const auto&) {
              calls.push_back(L"open-target");
              auto identity = ytec::diskmodel::make_stable_disk_identity(
                  target, target.is_system_disk);
              if (!identity) {
                return ytec::clonecore::Result<
                    ytec::diskmodel::PhysicalTargetHandle>::failure(
                    identity.error());
              }
              return ytec::clonecore::Result<
                  ytec::diskmodel::PhysicalTargetHandle>::success({
                  .observed = {
                      .target = target,
                      .target_identity = identity.take_value(),
                  },
                  .target = std::make_unique<FixtureTarget>(this),
              });
            },
    };
  }

  ytec::clonecore::Result<ytec::winpeapp::RescueCloneOperationPlan>
  prepare() {
    ytec::diskmodel::InventoryReport report;
    report.disks = {source, target};
    StaticInventory inventory(std::move(report));
    return ytec::winpeapp::prepare_rescue_clone_operation(
        source.disk_number,
        target.disk_number,
        inventory,
        [this](const auto&) {
          calls.push_back(L"preflight-active-media");
          return ytec::clonecore::Result<bool>::success(
              active_rescue_media);
        });
  }
};

std::uint64_t FixtureSource::size_bytes() const noexcept {
  return fixture_->source.size_bytes;
}

std::uint32_t FixtureSource::logical_sector_size() const noexcept {
  return fixture_->source.logical_sector_size;
}

ytec::clonecore::Result<std::vector<std::byte>> FixtureSource::read(
    const std::uint64_t offset,
    const std::size_t length) const {
  ++fixture_->source_read_count;
  const ReadKey key{offset, length};
  if (auto found = fixture_->recoverable_read_failures.find(key);
      found != fixture_->recoverable_read_failures.end() &&
      found->second != 0U) {
    --found->second;
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        injected_error(
            ytec::clonecore::ErrorCode::io_failed,
            ERROR_CRC,
            L"fixture source read"));
  }
  if (offset > fixture_->source_bytes.size() ||
      length > fixture_->source_bytes.size() - offset) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        injected_error(
            ytec::clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"fixture source bounds"));
  }
  return ytec::clonecore::Result<std::vector<std::byte>>::success(
      std::vector<std::byte>(
          fixture_->source_bytes.begin() +
              static_cast<std::ptrdiff_t>(offset),
          fixture_->source_bytes.begin() +
              static_cast<std::ptrdiff_t>(offset + length)));
}

std::uint64_t FixtureTarget::size_bytes() const noexcept {
  return fixture_->target.size_bytes;
}

std::uint32_t FixtureTarget::logical_sector_size() const noexcept {
  return fixture_->target.logical_sector_size;
}

ytec::clonecore::Status FixtureTarget::write_target(
    const std::uint64_t offset,
    const std::span<const std::byte> bytes) {
  ++fixture_->target_write_count;
  if (offset > fixture_->target_bytes.size() ||
      bytes.size() > fixture_->target_bytes.size() - offset) {
    return ytec::clonecore::Status::failure(injected_error(
        ytec::clonecore::ErrorCode::io_failed,
        ERROR_WRITE_FAULT,
        L"fixture target bounds"));
  }
  std::copy(
      bytes.begin(),
      bytes.end(),
      fixture_->target_bytes.begin() +
          static_cast<std::ptrdiff_t>(offset));
  return ytec::clonecore::success_status();
}

ytec::clonecore::Result<std::vector<std::byte>> FixtureTarget::read_back(
    const std::uint64_t offset,
    const std::size_t length) const {
  ++fixture_->target_read_back_count;
  if (offset > fixture_->target_bytes.size() ||
      length > fixture_->target_bytes.size() - offset) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        injected_error(
            ytec::clonecore::ErrorCode::io_failed,
            ERROR_READ_FAULT,
            L"fixture target readback bounds"));
  }
  std::vector<std::byte> bytes(
      fixture_->target_bytes.begin() +
          static_cast<std::ptrdiff_t>(offset),
      fixture_->target_bytes.begin() +
          static_cast<std::ptrdiff_t>(offset + length));
  if (fixture_->corrupt_target_read_back && !bytes.empty()) {
    bytes.front() ^= std::byte{1};
  }
  return ytec::clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

ytec::clonecore::Status FixtureTarget::flush_target() {
  ++fixture_->target_flush_count;
  return ytec::clonecore::success_status();
}

const ytec::winpeapp::RescueCloneExecutionReport& completed_report(
    const ytec::clonecore::Result<
        ytec::winpeapp::RescueCloneOperationReport>& result) {
  check(result.has_value(), "Operation wrapper should be returned");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::completed,
      "Rescue lifecycle should complete");
  check(result.value().rescue.has_value(), "Rescue evidence is required");
  return result.value().rescue.value();
}

void active_rescue_media_is_rejected_during_read_only_review() {
  RescueFixture fixture;
  fixture.active_rescue_media = true;
  auto prepared = fixture.prepare();
  check(!prepared.has_value(), "Active rescue media cannot be a target");
  check(
      prepared.error().code ==
          ytec::clonecore::ErrorCode::unsupported_layout,
      "Active media rejection should be a safety-layout error");
  check(
      fixture.source_read_count == 0U && fixture.target_write_count == 0U,
      "Read-only review must not enter either data path");
}

void pe_system_source_is_left_read_only_and_target_offline() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "PE review should allow a system-disk source");
  std::vector<ytec::clonecore::DiskOperationProgress> progress;
  auto dependencies = fixture.dependencies();
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(),
      true,
      L"OK",
      dependencies,
      {.progress = [&](const auto& value) { progress.push_back(value); }});
  const auto& report = completed_report(result);
  check(
      fixture.source.read_only.value_or(false),
      "PE rescue must leave the source read-only");
  check(
      fixture.target.offline.value_or(false),
      "PE rescue must leave the target offline");
  check(
      report.source_left_read_only && report.target_left_offline &&
          report.active_rescue_media_excluded_by_stable_identity &&
          report.must_display_as_partial_loss &&
          !report.shrinking_performed &&
          !report.partition_style_conversion_performed &&
          !report.boot_finalization_performed,
      "Product evidence must bind every rescue-only safety invariant");
  check(
      !report.raw.partial_data_loss && report.raw.missing_ranges.empty() &&
          report.raw.byte_exact_copy,
      "Actual missing-range truth must remain separate from UI classification");
  const std::wstring presentation =
      ytec::winpeapp::format_rescue_clone_product_result(report);
  check(
      presentation.starts_with(
          ytec::winpeapp::kRescueCloneResultClassification) &&
          presentation.find(L"実欠損map: 0件") != std::wstring::npos &&
          presentation.find(L"通常クローン成功・起動成功の表示ではありません") !=
              std::wstring::npos,
      "The PE UI text must keep conservative classification separate from a zero-loss map");
  check(
      std::equal(
          fixture.source_bytes.begin(),
          fixture.source_bytes.end(),
          fixture.target_bytes.begin()) &&
          fixture.target_read_back_count == fixture.target_write_count &&
          fixture.target_flush_count == 1U,
      "Every source byte must be written, read back, and flushed");
  check(
      !progress.empty() &&
          progress.back().stage ==
              ytec::clonecore::DiskOperationStage::completed,
      "Rescue progress must reach the product progress surface");
}

void unrecognized_or_empty_source_layout_is_rescued_as_raw() {
  RescueFixture fixture;
  fixture.source.partition_style =
      ytec::diskmodel::PartitionStyle::unknown;
  fixture.source.partitions.clear();
  auto prepared = fixture.prepare();
  check(
      prepared.has_value() &&
          prepared.value().source_partition_style ==
              ytec::diskmodel::PartitionStyle::unknown &&
          prepared.value().source_partition_count == 0U,
      "A damaged or unrecognized partition table must remain eligible for RAW rescue");
  auto dependencies = fixture.dependencies();
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.raw.layout_preserved_without_conversion &&
          !report.shrinking_performed &&
          !report.partition_style_conversion_performed &&
          !report.boot_finalization_performed,
      "Unrecognized layouts must be copied only as exact RAW extents");
}

void failing_source_health_is_allowed_only_as_a_rescue_source() {
  RescueFixture fixture;
  fixture.source.health.state =
      ytec::diskmodel::DiskHealthState::failing;
  auto prepared = fixture.prepare();
  check(
      prepared.has_value() &&
          prepared.value().source_health.state ==
              ytec::diskmodel::DiskHealthState::failing,
      "A failing source must remain eligible for the explicit PE rescue path");
  check(
      fixture.source_read_count == 0U && fixture.target_write_count == 0U,
      "Rescue health review must stay read-only");
}

void larger_target_tail_is_not_presented_as_erased_or_normal_success() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Rescue review should allow a larger target");
  auto dependencies = fixture.dependencies();
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.untouched_target_tail_bytes ==
              fixture.target.size_bytes - fixture.source.size_bytes &&
          report.must_display_as_partial_loss,
      "A larger target must report its untouched tail and remain a rescue result");
  const std::wstring presentation =
      ytec::winpeapp::format_rescue_clone_product_result(report);
  check(
      presentation.find(L"コピー先の未処理末尾: 4096 bytes") !=
              std::wstring::npos &&
          presentation.find(L"消去・検証していません") !=
              std::wstring::npos,
      "The PE result must explicitly disclose an untouched larger-target tail");
  check(
      std::all_of(
          fixture.target_bytes.begin() +
              static_cast<std::ptrdiff_t>(fixture.source.size_bytes),
          fixture.target_bytes.end(),
          [](const std::byte value) { return value == std::byte{0x7F}; }),
      "The product evidence must not claim the untouched target tail was erased");
}

void four_kn_rescue_is_currently_rejected_before_mutation() {
  RescueFixture fixture;
  fixture.source.logical_sector_size = 4096U;
  fixture.source.physical_sector_size = 4096U;
  fixture.source.sector_count = 1U;
  fixture.target.logical_sector_size = 4096U;
  fixture.target.physical_sector_size = 4096U;
  fixture.target.sector_count = 2U;
  auto prepared = fixture.prepare();
  check(
      !prepared.has_value() && fixture.source_read_count == 0U &&
          fixture.target_write_count == 0U,
      "The current 4Kn product boundary must fail closed during read-only review");
}

void unreadable_sector_is_zero_filled_and_mapped() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Rescue review should succeed");
  fixture.recoverable_read_failures[{0U, 4096U}] = 2U;
  fixture.recoverable_read_failures[{512U, 512U}] = 1U;
  auto dependencies = fixture.dependencies();
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.must_display_as_partial_loss && report.raw.partial_data_loss &&
          report.raw.zero_filled_bytes == 512U &&
          report.raw.missing_ranges.size() == 1U &&
          report.raw.missing_ranges.front().bytes.offset == 512U &&
          report.raw.missing_ranges.front().bytes.length == 512U,
      "Actual exhausted sectors need an exact separate missing map");
  const std::wstring presentation =
      ytec::winpeapp::format_rescue_clone_product_result(report);
  check(
      presentation.starts_with(
          ytec::winpeapp::kRescueCloneResultClassification) &&
          presentation.find(L"実欠損map: 1件 / 512 bytes") !=
              std::wstring::npos &&
          presentation.find(L"offset 512 / length 512 / LBA 1") !=
              std::wstring::npos,
      "The PE UI text must expose the actual zero-filled range separately");
  check(
      std::all_of(
          fixture.target_bytes.begin() + 512,
          fixture.target_bytes.begin() + 1024,
          [](const std::byte value) { return value == std::byte{0}; }),
      "Only the unreadable sector should be zero-filled");
}

void exact_ok_is_required_before_any_mutation() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Rescue review should succeed");
  auto dependencies = fixture.dependencies();
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(), true, L"ok", dependencies);
  check(result.has_value(), "Lifecycle evidence should be returned");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::failed &&
          result.value().lifecycle.error.has_value() &&
          result.value().lifecycle.error->code ==
              ytec::clonecore::ErrorCode::confirmation_required,
      "Lowercase confirmation must fail in OperationCore");
  check(
      !fixture.source.read_only.value_or(true) &&
          !fixture.target.offline.value_or(true) &&
          fixture.target_write_count == 0U,
      "Bad confirmation must stop before source or target mutation");
}

void changed_layout_or_new_active_media_state_stops_before_mutation() {
  {
    RescueFixture fixture;
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    fixture.source.partitions.front().size_bytes -= 512U;
    auto dependencies = fixture.dependencies();
    auto result = ytec::winpeapp::execute_rescue_clone_operation(
        prepared.value(), true, L"OK", dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            !fixture.source.read_only.value_or(true) &&
            fixture.target_write_count == 0U,
        "A changed source layout must fail before mutation");
  }
  {
    RescueFixture fixture;
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    fixture.active_rescue_media = true;
    auto dependencies = fixture.dependencies();
    auto result = ytec::winpeapp::execute_rescue_clone_operation(
        prepared.value(), true, L"OK", dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            !fixture.source.read_only.value_or(true) &&
            fixture.target_write_count == 0U,
        "The active rescue media check must be repeated before mutation");
  }
}

void target_readback_failure_reasserts_offline_protection() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Rescue review should succeed");
  fixture.corrupt_target_read_back = true;
  auto dependencies = fixture.dependencies();
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(), true, L"OK", dependencies);
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed,
      "Read-back corruption must fail the lifecycle");
  check(
      fixture.source.read_only.value_or(false) &&
          fixture.target.offline.value_or(false) &&
          fixture.target_flush_count == 1U,
      "A failed destructive rescue must leave source read-only and target offline");
  check(
      std::count(
          fixture.calls.begin(),
          fixture.calls.end(),
          std::wstring(L"target-offline")) >= 2,
      "Failure cleanup must explicitly reassert target offline state");
}

void cancellation_keeps_both_disk_protection_boundaries() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Rescue review should succeed");
  auto dependencies = fixture.dependencies();
  std::size_t cancellation_checks = 0U;
  auto result = ytec::winpeapp::execute_rescue_clone_operation(
      prepared.value(),
      true,
      L"OK",
      dependencies,
      {.cancellation_requested = [&cancellation_checks]() {
         ++cancellation_checks;
         return cancellation_checks >= 3U;
       }});
  check(
      result.has_value() &&
          result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::cancelled &&
          !result.value().rescue.has_value(),
      "Cancellation must not be returned as a completed rescue result");
  check(
      fixture.source.read_only.value_or(false) &&
          fixture.target.offline.value_or(false) &&
          fixture.target_write_count == 0U,
      "Cancellation must retain source read-only and target offline boundaries");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
      {"active_rescue_media_is_rejected_during_read_only_review",
       active_rescue_media_is_rejected_during_read_only_review},
      {"pe_system_source_is_left_read_only_and_target_offline",
       pe_system_source_is_left_read_only_and_target_offline},
      {"unrecognized_or_empty_source_layout_is_rescued_as_raw",
       unrecognized_or_empty_source_layout_is_rescued_as_raw},
      {"failing_source_health_is_allowed_only_as_a_rescue_source",
       failing_source_health_is_allowed_only_as_a_rescue_source},
      {"larger_target_tail_is_not_presented_as_erased_or_normal_success",
       larger_target_tail_is_not_presented_as_erased_or_normal_success},
      {"four_kn_rescue_is_currently_rejected_before_mutation",
       four_kn_rescue_is_currently_rejected_before_mutation},
      {"unreadable_sector_is_zero_filled_and_mapped",
       unreadable_sector_is_zero_filled_and_mapped},
      {"exact_ok_is_required_before_any_mutation",
       exact_ok_is_required_before_any_mutation},
      {"changed_layout_or_new_active_media_state_stops_before_mutation",
       changed_layout_or_new_active_media_state_stops_before_mutation},
      {"target_readback_failure_reasserts_offline_protection",
       target_readback_failure_reasserts_offline_protection},
      {"cancellation_keeps_both_disk_protection_boundaries",
       cancellation_keeps_both_disk_protection_boundaries},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

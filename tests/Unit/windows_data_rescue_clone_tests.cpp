#include "ytec/windowsapp/windows_data_rescue_clone.h"

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

template <typename T>
concept HasSourceAttributeSetter = requires(T dependencies) {
  dependencies.set_source_read_only;
};

static_assert(
    !HasSourceAttributeSetter<
        ytec::windowsapp::WindowsDataRescueCloneDependencies>,
    "The running-Windows rescue service must not expose a source-attribute writer");

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

ytec::diskmodel::DiskInfo make_source_disk() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 0U;
  disk.device_path = L"\\\\.\\PhysicalDrive0";
  disk.device_instance_id = L"SYNTHETIC\\WINDOWS-RESCUE\\SOURCE";
  disk.model = L"WINDOWS RESCUE SOURCE";
  disk.size_bytes = 4096U;
  disk.sector_count = 8U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 512U;
  disk.bus_type = L"SATA";
  disk.serial_suffix = "SOURCE01";
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.is_system_disk = false;
  disk.offline = false;
  disk.read_only = true;
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
  disk.device_instance_id = L"SYNTHETIC\\WINDOWS-RESCUE\\TARGET";
  disk.model = L"WINDOWS RESCUE TARGET";
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
  std::size_t protected_query_count{};
  std::size_t source_open_count{};
  std::size_t source_read_count{};
  std::size_t target_write_count{};
  std::size_t target_read_back_count{};
  std::size_t target_flush_count{};
  bool protected_rescue_media{};
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

  ytec::windowsapp::WindowsDataRescueCloneDependencies dependencies() {
    return ytec::windowsapp::WindowsDataRescueCloneDependencies{
        .reidentify_selection =
            [this](const auto& source_identity,
                   const auto& target_identity) {
              return reidentify(source_identity, target_identity);
            },
        .is_protected_rescue_media =
            [this](const auto&) {
              ++protected_query_count;
              calls.push_back(L"protected-media");
              return ytec::clonecore::Result<bool>::success(
                  protected_rescue_media);
            },
        .open_read_only_source =
            [this](const auto&) {
              ++source_open_count;
              calls.push_back(L"open-source-read-only");
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
              calls.push_back(
                  offline ? L"target-offline" : L"target-online");
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

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsDataRescueClonePlan>
  prepare() {
    ytec::diskmodel::InventoryReport report;
    report.disks = {source, target};
    StaticInventory inventory(std::move(report));
    return ytec::windowsapp::prepare_windows_data_rescue_clone(
        source.disk_number,
        target.disk_number,
        inventory,
        [this](const auto&) {
          ++protected_query_count;
          calls.push_back(L"preflight-protected-media");
          return ytec::clonecore::Result<bool>::success(
              protected_rescue_media);
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

const ytec::windowsapp::WindowsDataRescueCloneExecutionReport&
completed_report(
    const ytec::clonecore::Result<
        ytec::windowsapp::WindowsDataRescueCloneOperationReport>& result) {
  check(result.has_value(), "Operation wrapper should be returned");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::completed,
      "Rescue lifecycle should complete");
  check(result.value().rescue.has_value(), "Rescue evidence is required");
  return result.value().rescue.value();
}

void running_system_source_is_rejected_before_raw_io() {
  RescueFixture fixture;
  fixture.source.is_system_disk = true;
  const bool initial_read_only = fixture.source.read_only.value();
  const bool initial_offline = fixture.source.offline.value();
  auto prepared = fixture.prepare();
  check(!prepared.has_value(), "A running system source must be rejected");
  check(
      prepared.error().code ==
          ytec::clonecore::ErrorCode::unsupported_layout,
      "System-source rejection should use the explicit PE boundary");
  check(
      fixture.protected_query_count == 0U &&
          fixture.source_open_count == 0U &&
          fixture.source_read_count == 0U &&
          fixture.target_write_count == 0U,
      "System-source rejection must precede protection queries and raw I/O");
  check(
      fixture.source.read_only.value() == initial_read_only &&
          fixture.source.offline.value() == initial_offline,
      "System-source rejection must not change source attributes");
}

void writable_online_source_fails_closed_without_attribute_mutation() {
  RescueFixture fixture;
  fixture.source.read_only = false;
  fixture.source.offline = false;
  auto prepared = fixture.prepare();
  check(
      !prepared.has_value() && fixture.source_open_count == 0U &&
          fixture.source_read_count == 0U && fixture.target_write_count == 0U,
      "An online writable data disk must be sent to PE before raw I/O");
  check(
      !fixture.source.read_only.value() && !fixture.source.offline.value(),
      "Windows rescue must never transition source disk attributes");
}

void protected_data_source_completes_without_source_attribute_write() {
  RescueFixture fixture;
  const bool initial_read_only = fixture.source.read_only.value();
  const bool initial_offline = fixture.source.offline.value();
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Protected non-system data source should review");
  auto dependencies = fixture.dependencies();
  auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.source_opened_read_only &&
          report.source_was_read_only_or_offline &&
          report.source_attributes_unchanged && report.target_left_offline &&
          report.protected_rescue_media_excluded_by_stable_identity &&
          report.must_display_as_partial_loss &&
          !report.shrinking_performed &&
          !report.partition_style_conversion_performed &&
          !report.boot_finalization_performed,
      "Every Windows rescue-only safety invariant needs evidence");
  check(
      fixture.source.read_only.value() == initial_read_only &&
          fixture.source.offline.value() == initial_offline &&
          std::find(
              fixture.calls.begin(),
              fixture.calls.end(),
              std::wstring(L"source-attribute-write")) ==
              fixture.calls.end(),
      "No source attribute modification callback may run");
  check(
      !report.raw.partial_data_loss && report.raw.missing_ranges.empty() &&
          report.raw.byte_exact_copy,
      "Actual missing-map truth must remain separate from classification");
  const std::wstring presentation =
      ytec::windowsapp::format_windows_data_rescue_clone_result(report);
  check(
      presentation.starts_with(
          ytec::windowsapp::kWindowsDataRescueCloneResultClassification) &&
          presentation.find(L"実欠損map: 0件") != std::wstring::npos &&
          presentation.find(L"コピー元属性の非変更: 確認済み") !=
              std::wstring::npos &&
          presentation.find(L"通常クローン成功・起動成功の表示ではありません") !=
              std::wstring::npos,
      "Product text must separate conservative classification and actual map");
  check(
      std::equal(
          fixture.source_bytes.begin(),
          fixture.source_bytes.end(),
          fixture.target_bytes.begin()) &&
          fixture.target_read_back_count == fixture.target_write_count &&
          fixture.target_flush_count == 1U,
      "Every written byte must be read back and the target flushed");
}

void raw_unknown_source_layout_is_eligible() {
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
      "Damaged or unrecognized source layouts must remain RAW-rescuable");
  auto dependencies = fixture.dependencies();
  auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.raw.layout_preserved_without_conversion &&
          !report.shrinking_performed &&
          !report.partition_style_conversion_performed &&
          !report.boot_finalization_performed,
      "Unknown source layouts may only use whole-disk RAW rescue");
}

void unsafe_targets_are_rejected_during_review() {
  {
    RescueFixture fixture;
    fixture.protected_rescue_media = true;
    auto prepared = fixture.prepare();
    check(
        !prepared.has_value() && fixture.source_open_count == 0U &&
            fixture.target_write_count == 0U,
        "The active rescue media must be excluded at review");
  }
  {
    RescueFixture fixture;
    fixture.target.bus_type = L"USB";
    fixture.target.removable = true;
    auto prepared = fixture.prepare();
    check(
        !prepared.has_value() && fixture.source_open_count == 0U &&
            fixture.target_write_count == 0U,
        "USB memory must never be a rescue target");
  }
}

void four_kn_is_rejected_before_mutation() {
  RescueFixture fixture;
  fixture.source.logical_sector_size = 4096U;
  fixture.source.physical_sector_size = 4096U;
  fixture.source.sector_count = 1U;
  fixture.target.logical_sector_size = 4096U;
  fixture.target.physical_sector_size = 4096U;
  fixture.target.sector_count = 2U;
  auto prepared = fixture.prepare();
  check(
      !prepared.has_value() && fixture.source_open_count == 0U &&
          fixture.source_read_count == 0U && fixture.target_write_count == 0U,
      "Current 4Kn boundary must fail closed at read-only review");
}

void exact_ok_and_fresh_observation_are_required() {
  {
    RescueFixture fixture;
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    auto dependencies = fixture.dependencies();
    auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
        prepared.value(), true, L"ok", dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            result.value().lifecycle.error.has_value() &&
            result.value().lifecycle.error->code ==
                ytec::clonecore::ErrorCode::confirmation_required &&
            fixture.source_open_count == 0U &&
            fixture.target_write_count == 0U,
        "Only exact uppercase OK may cross into destructive execution");
  }
  {
    RescueFixture fixture;
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    fixture.source.partitions.front().size_bytes -= 512U;
    auto dependencies = fixture.dependencies();
    auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
        prepared.value(), true, L"OK", dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            fixture.source_open_count == 0U &&
            fixture.target_write_count == 0U,
        "A changed layout must stop before raw I/O");
  }
  {
    RescueFixture fixture;
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    fixture.protected_rescue_media = true;
    auto dependencies = fixture.dependencies();
    auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
        prepared.value(), true, L"OK", dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            fixture.protected_query_count >= 2U &&
            fixture.source_open_count == 0U &&
            fixture.target_write_count == 0U,
        "Protected rescue-media state must be queried again before I/O");
  }
}

void unreadable_sector_is_zero_filled_and_mapped() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Rescue review should succeed");
  fixture.recoverable_read_failures[{0U, 4096U}] = 2U;
  fixture.recoverable_read_failures[{512U, 512U}] = 1U;
  auto dependencies = fixture.dependencies();
  auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.must_display_as_partial_loss && report.raw.partial_data_loss &&
          report.raw.zero_filled_bytes == 512U &&
          report.raw.missing_ranges.size() == 1U &&
          report.raw.missing_ranges.front().bytes.offset == 512U &&
          report.raw.missing_ranges.front().bytes.length == 512U,
      "Exhausted sectors need a precise actual missing map");
  const std::wstring presentation =
      ytec::windowsapp::format_windows_data_rescue_clone_result(report);
  check(
      presentation.starts_with(
          ytec::windowsapp::kWindowsDataRescueCloneResultClassification) &&
          presentation.find(L"実欠損map: 1件 / 512 bytes") !=
              std::wstring::npos &&
          presentation.find(L"offset 512 / length 512 / LBA 1") !=
              std::wstring::npos,
      "Actual zero-filled ranges must be rendered separately");
  check(
      std::all_of(
          fixture.target_bytes.begin() + 512,
          fixture.target_bytes.begin() + 1024,
          [](const std::byte value) { return value == std::byte{0}; }),
      "Only the unreadable sector should be zero-filled");
}

void larger_target_tail_remains_untouched_and_disclosed() {
  RescueFixture fixture;
  auto prepared = fixture.prepare();
  check(prepared.has_value(), "Larger target should review");
  auto dependencies = fixture.dependencies();
  auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
      prepared.value(), true, L"OK", dependencies);
  const auto& report = completed_report(result);
  check(
      report.untouched_target_tail_bytes ==
          fixture.target.size_bytes - fixture.source.size_bytes,
      "Untouched target tail needs explicit evidence");
  const std::wstring presentation =
      ytec::windowsapp::format_windows_data_rescue_clone_result(report);
  check(
      presentation.find(L"コピー先の未処理末尾: 4096 bytes") !=
              std::wstring::npos &&
          presentation.find(L"消去・検証していません") !=
              std::wstring::npos,
      "The larger-target tail must not be presented as completed output");
  check(
      std::all_of(
          fixture.target_bytes.begin() +
              static_cast<std::ptrdiff_t>(fixture.source.size_bytes),
          fixture.target_bytes.end(),
          [](const std::byte value) { return value == std::byte{0x7F}; }),
      "The target tail must remain physically untouched in the fixture");
}

void failure_and_cancellation_keep_target_offline_and_source_unchanged() {
  {
    RescueFixture fixture;
    const bool initial_read_only = fixture.source.read_only.value();
    const bool initial_offline = fixture.source.offline.value();
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    fixture.corrupt_target_read_back = true;
    auto dependencies = fixture.dependencies();
    auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
        prepared.value(), true, L"OK", dependencies);
    check(
        result.has_value() &&
            result.value().lifecycle.outcome ==
                ytec::operationcore::OperationOutcome::failed &&
            fixture.target.offline.value_or(false) &&
            fixture.source.read_only.value() == initial_read_only &&
            fixture.source.offline.value() == initial_offline,
        "Read-back failure must retain target offline and source attributes");
    check(
        std::count(
            fixture.calls.begin(),
            fixture.calls.end(),
            std::wstring(L"target-offline")) >= 2,
        "Failure cleanup must reassert target offline");
  }
  {
    RescueFixture fixture;
    const bool initial_read_only = fixture.source.read_only.value();
    const bool initial_offline = fixture.source.offline.value();
    auto prepared = fixture.prepare();
    check(prepared.has_value(), "Rescue review should succeed");
    auto dependencies = fixture.dependencies();
    std::size_t cancellation_checks = 0U;
    auto result = ytec::windowsapp::execute_windows_data_rescue_clone(
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
            !result.value().rescue.has_value() &&
            fixture.target.offline.value_or(false) &&
            fixture.source.read_only.value() == initial_read_only &&
            fixture.source.offline.value() == initial_offline,
        "Cancellation must retain target offline and source attributes");
  }
}

void marked_rescue_media_resolver_is_stable_and_fail_closed() {
  const auto target = make_target_disk();
  auto expected = ytec::diskmodel::make_stable_disk_identity(target, false);
  check(expected.has_value(), "Target identity fixture should be valid");
  constexpr std::string_view kMarker =
      "12345678-1234-1234-1234-123456789abc";

  auto make_dependencies = [&](const bool marker_present) {
    return ytec::windowsapp::
        WindowsDataRescueProtectedTargetDependencies{
            .running_in_winpe = []() {
              return ytec::clonecore::Result<bool>::success(false);
            },
            .enumerate_mounted_volumes = []() {
              return ytec::clonecore::Result<std::vector<
                  ytec::windowsapp::WindowsDataRescueMountedVolume>>::success({
                  {
                      .root_path = L"R:\\",
                      .disk_numbers = {1U},
                  },
              });
            },
            .read_rescue_marker =
                [marker_present](const std::wstring&) {
                  return ytec::clonecore::Result<
                      std::optional<std::string>>::success(
                      marker_present
                          ? std::optional<std::string>{
                                std::string(kMarker)}
                          : std::nullopt);
                },
            .enumerate_disks = [target]() {
              ytec::diskmodel::InventoryReport inventory;
              inventory.disks.push_back(target);
              return ytec::clonecore::Result<
                  ytec::diskmodel::InventoryReport>::success(
                  std::move(inventory));
            },
        };
  };

  auto unmarked =
      ytec::windowsapp::resolve_windows_data_rescue_protected_target(
          expected.value(), make_dependencies(false));
  check(
      unmarked.has_value() && !unmarked.value(),
      "A stable unmarked target should remain eligible");
  auto marked =
      ytec::windowsapp::resolve_windows_data_rescue_protected_target(
          expected.value(), make_dependencies(true));
  check(
      marked.has_value() && marked.value(),
      "A stable marked Y-TEC rescue medium must be protected");

  auto malformed_dependencies = make_dependencies(false);
  malformed_dependencies.read_rescue_marker = [](const std::wstring&) {
    return ytec::clonecore::Result<std::optional<std::string>>::success(
        std::string("not-a-valid-rescue-marker"));
  };
  auto malformed =
      ytec::windowsapp::resolve_windows_data_rescue_protected_target(
          expected.value(), malformed_dependencies);
  check(
      !malformed.has_value(),
      "Malformed target marker evidence must fail closed");

  std::size_t volume_observations = 0U;
  auto drift_dependencies = make_dependencies(false);
  drift_dependencies.enumerate_mounted_volumes = [&volume_observations]() {
    ++volume_observations;
    std::vector<ytec::windowsapp::WindowsDataRescueMountedVolume> volumes;
    if (volume_observations == 1U) {
      volumes.push_back({.root_path = L"R:\\", .disk_numbers = {1U}});
    }
    return ytec::clonecore::Result<std::vector<
        ytec::windowsapp::WindowsDataRescueMountedVolume>>::success(
        std::move(volumes));
  };
  auto drift =
      ytec::windowsapp::resolve_windows_data_rescue_protected_target(
          expected.value(), drift_dependencies);
  check(
      !drift.has_value(),
      "A target-volume mapping change during review must fail closed");

  auto winpe_dependencies = make_dependencies(false);
  winpe_dependencies.running_in_winpe = []() {
    return ytec::clonecore::Result<bool>::success(true);
  };
  auto wrong_environment =
      ytec::windowsapp::resolve_windows_data_rescue_protected_target(
          expected.value(), winpe_dependencies);
  check(
      !wrong_environment.has_value(),
      "The running-Windows rescue controller must reject WinPE");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
      {"running_system_source_is_rejected_before_raw_io",
       running_system_source_is_rejected_before_raw_io},
      {"writable_online_source_fails_closed_without_attribute_mutation",
       writable_online_source_fails_closed_without_attribute_mutation},
      {"protected_data_source_completes_without_source_attribute_write",
       protected_data_source_completes_without_source_attribute_write},
      {"raw_unknown_source_layout_is_eligible",
       raw_unknown_source_layout_is_eligible},
      {"unsafe_targets_are_rejected_during_review",
       unsafe_targets_are_rejected_during_review},
      {"four_kn_is_rejected_before_mutation",
       four_kn_is_rejected_before_mutation},
      {"exact_ok_and_fresh_observation_are_required",
       exact_ok_and_fresh_observation_are_required},
      {"unreadable_sector_is_zero_filled_and_mapped",
       unreadable_sector_is_zero_filled_and_mapped},
      {"larger_target_tail_remains_untouched_and_disclosed",
       larger_target_tail_remains_untouched_and_disclosed},
      {"failure_and_cancellation_keep_target_offline_and_source_unchanged",
       failure_and_cancellation_keep_target_offline_and_source_unchanged},
      {"marked_rescue_media_resolver_is_stable_and_fail_closed",
       marked_rescue_media_resolver_is_stable_and_fail_closed},
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

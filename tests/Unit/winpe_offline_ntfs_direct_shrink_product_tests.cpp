#include "ytec/winpeapp/offline_ntfs_direct_shrink_product.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::wstring_view kWindowsVolume =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
constexpr std::wstring_view kRecoveryVolume =
    L"\\\\?\\Volume{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\\";
constexpr std::wstring_view kEmptyDataVolume =
    L"\\\\?\\Volume{12345678-1234-5678-9ABC-123456789ABC}\\";

#if !defined(YTEC_WINPE_GUI_SOURCE_PATH)
#error YTEC_WINPE_GUI_SOURCE_PATH must name the product WinPE GUI source.
#endif

#if !defined(YTEC_WINPE_DIRECT_SHRINK_PRODUCT_SOURCE_PATH)
#error YTEC_WINPE_DIRECT_SHRINK_PRODUCT_SOURCE_PATH must name the product adapter source.
#endif

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <std::size_t Size>
std::array<std::byte, Size> filled(const unsigned char value) {
  std::array<std::byte, Size> result{};
  result.fill(static_cast<std::byte>(value));
  return result;
}

ytec::diskmodel::PartitionInfo gpt_partition(
    const std::uint32_t number,
    const std::uint64_t offset,
    const std::uint64_t size,
    std::wstring type,
    std::wstring name) {
  return ytec::diskmodel::PartitionInfo{
      .number = number,
      .offset_bytes = offset,
      .size_bytes = size,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = std::move(type),
      .identifier = L"GPT-" + std::to_wstring(number),
      .name = std::move(name),
  };
}

ytec::diskmodel::DiskInfo source_disk() {
  constexpr std::uint64_t esp_offset = 1ULL * kMiB;
  constexpr std::uint64_t esp_size = 260ULL * kMiB;
  constexpr std::uint64_t msr_offset = esp_offset + esp_size;
  constexpr std::uint64_t msr_size = 16ULL * kMiB;
  constexpr std::uint64_t windows_offset = msr_offset + msr_size;
  constexpr std::uint64_t windows_size = 40ULL * kGiB;
  constexpr std::uint64_t recovery_offset = windows_offset + windows_size;
  constexpr std::uint64_t recovery_size = 1ULL * kGiB;
  constexpr std::uint64_t data_offset = recovery_offset + recovery_size;
  constexpr std::uint64_t data_size = 8ULL * kGiB;
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 2U,
      .device_path = L"\\\\.\\PhysicalDrive2",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Source",
      .connection_location_path = L"PCIROOT(0)#PCI(0100)",
      .device_instance_id =
          L"SCSI\\DISK&VEN_YTEC&PROD_SOURCE\\SOURCE-A",
      .model = L"YTEC SYNTHETIC WINPE SOURCE",
      .size_bytes = 64ULL * kGiB,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "SOURCE01",
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .disk_identifier = L"{11111111-2222-3333-4444-555555555555}",
      .offline = false,
      .read_only = true,
      .removable = false,
      .is_system_disk = false,
      .partitions = {
          gpt_partition(1U, esp_offset, esp_size, L"EFI System", L"SYSTEM"),
          gpt_partition(2U, msr_offset, msr_size, L"MSR", L""),
          gpt_partition(
              3U, windows_offset, windows_size, L"Basic data", L"Windows"),
          gpt_partition(
              4U,
              recovery_offset,
              recovery_size,
              L"Windows Recovery",
              L"Recovery"),
          gpt_partition(
              5U, data_offset, data_size, L"Basic data", L"Empty data"),
      },
  };
  disk.sector_count = disk.size_bytes / disk.logical_sector_size;
  return disk;
}

ytec::diskmodel::DiskInfo target_disk() {
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 5U,
      .device_path = L"\\\\.\\PhysicalDrive5",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Target",
      .connection_location_path = L"PCIROOT(0)#PCI(0200)",
      .device_instance_id =
          L"SCSI\\DISK&VEN_YTEC&PROD_TARGET\\TARGET-A",
      .model = L"YTEC SYNTHETIC WINPE TARGET",
      .size_bytes = 24ULL * kGiB,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "TARGET01",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .disk_identifier = L"RAW",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.sector_count = disk.size_bytes / disk.logical_sector_size;
  return disk;
}

struct Fixture final {
  ytec::winpeapp::WinPeOfflineNtfsProductPlanningRequest request;
  ytec::windowsshrink::ShrinkSourceAnalysis analysis;
};

Fixture fixture() {
  constexpr std::uint64_t esp_offset = 1ULL * kMiB;
  constexpr std::uint64_t esp_size = 260ULL * kMiB;
  constexpr std::uint64_t msr_offset = esp_offset + esp_size;
  constexpr std::uint64_t msr_size = 16ULL * kMiB;
  constexpr std::uint64_t windows_offset = msr_offset + msr_size;
  constexpr std::uint64_t windows_size = 40ULL * kGiB;
  constexpr std::uint64_t recovery_offset = windows_offset + windows_size;
  constexpr std::uint64_t recovery_size = 1ULL * kGiB;
  constexpr std::uint64_t data_offset = recovery_offset + recovery_size;
  constexpr std::uint64_t data_size = 8ULL * kGiB;
  auto source = source_disk();
  auto target = target_disk();
  auto stable = ytec::diskmodel::make_stable_disk_identity(source, false);
  check(stable.has_value(), "Synthetic WinPE source identity must build");
  ytec::windowsshrink::ShrinkSourceAnalysis analysis{
      .source = stable.take_value(),
      .physical_sector_size = source.physical_sector_size,
      .partition_style = ytec::migrationcore::MigrationPartitionStyle::gpt,
      .windows_version = ytec::windowsshrink::WindowsSourceVersion{
          .major = 10U,
          .minor = 0U,
          .build = 22631U,
          .architecture = "AMD64",
      },
      .bitlocker_fully_decrypted = true,
      .created_utc = "2026-08-23T00:00:00Z",
      .app_version = "0.2.0-dev",
      .partition_snapshot = std::vector<std::byte>(4096U, std::byte{0x5A}),
      .partitions = {
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 1U,
              .role = ytec::migrationcore::MigrationPartitionRole::efi_system,
              .file_system =
                  ytec::migrationcore::MigrationFileSystem::unsupported,
              .source_offset_bytes = esp_offset,
              .source_size_bytes = esp_size,
              .used_bytes = esp_size,
              .name = L"SYSTEM",
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 2U,
              .role = ytec::migrationcore::MigrationPartitionRole::
                  microsoft_reserved,
              .file_system = ytec::migrationcore::MigrationFileSystem::none,
              .source_offset_bytes = msr_offset,
              .source_size_bytes = msr_size,
              .used_bytes = msr_size,
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 3U,
              .role = ytec::migrationcore::MigrationPartitionRole::windows,
              .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
              .source_offset_bytes = windows_offset,
              .source_size_bytes = windows_size,
              .used_bytes = 8ULL * kGiB,
              .cluster_size = 4096U,
              .label = L"Windows",
              .name = L"Windows",
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 4U,
              .role = ytec::migrationcore::MigrationPartitionRole::recovery,
              .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
              .source_offset_bytes = recovery_offset,
              .source_size_bytes = recovery_size,
              .used_bytes = 700ULL * kMiB,
              .cluster_size = 4096U,
              .label = L"Recovery",
              .name = L"Recovery",
          },
          ytec::windowsshrink::AnalyzedShrinkPartition{
              .source_table_index = 5U,
              .role = ytec::migrationcore::MigrationPartitionRole::data,
              .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
              .source_offset_bytes = data_offset,
              .source_size_bytes = data_size,
              .used_bytes = 0U,
              .cluster_size = 4096U,
              .label = L"Empty data",
              .name = L"Empty data",
          },
      },
      .content_volumes = {
          {3U, std::wstring(kWindowsVolume)},
          {4U, std::wstring(kRecoveryVolume)},
          {5U, std::wstring(kEmptyDataVolume)},
      },
  };
  return Fixture{
      .request = ytec::winpeapp::WinPeOfflineNtfsProductPlanningRequest{
          .administrator = true,
          .winpe_environment_verified = true,
          .reviewed_source = std::move(source),
          .reviewed_target = std::move(target),
          .operation_id = filled<16U>(0x71U),
          .surplus_allocation =
              ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
          .analysis_created_utc = "2026-08-23T00:00:00Z",
          .app_version = "0.2.0-dev",
      },
      .analysis = std::move(analysis),
  };
}

const ytec::directshrink::PartitionTask& task_for_source(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan& plan,
    const std::uint32_t source_table_index) {
  const auto found = std::find_if(
      plan.target_plan().tasks().begin(),
      plan.target_plan().tasks().end(),
      [source_table_index](const auto& task) {
        return task.source_table_index == source_table_index;
      });
  if (found == plan.target_plan().tasks().end()) {
    throw std::runtime_error("Expected source-bound target task was not found");
  }
  return *found;
}

const ytec::directshrink::SourcePartitionMapping& mapping_for_source(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan& plan,
    const std::uint32_t source_table_index) {
  const auto mappings = plan.target_plan().source_partition_mappings();
  const auto found = std::find_if(
      mappings.begin(),
      mappings.end(),
      [source_table_index](const auto& mapping) {
        return mapping.source_table_index == source_table_index;
      });
  if (found == mappings.end()) {
    throw std::runtime_error("Expected source partition mapping was not found");
  }
  return *found;
}

void test_review_and_reanalysis_build_source_bound_empty_ntfs() {
  auto data = fixture();
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The synthetic WinPE review must succeed");
  check(review.value().candidates.size() == 5U,
        "All source partition-table entries must be reviewed");

  const auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(plan.has_value(), "The unchanged reviewed analysis must plan");
  check(plan.value().target_plan().tasks().size() == 5U,
        "The five-partition GPT plan must survive the shared target contract");
  check(plan.value().target_plan().archive_task_count() == 2U,
        "Only non-empty NTFS partitions may require WIM archives");
  const auto& empty = task_for_source(plan.value(), 5U);
  check(empty.kind == ytec::directshrink::PartitionTaskKind::create_empty_ntfs &&
            empty.source_used_bytes == 0U &&
            empty.original_volume_guid_path.empty(),
        "The selected empty data partition must be recreated without WIM capture");
  check(plan.value().planning_evidence().source_epoch.ntfs_volumes.size() == 2U,
        "The execution epoch must retain only WIM-captured source volumes");
}

void test_selected_empty_data_can_receive_surplus() {
  auto data = fixture();
  data.request.surplus_allocation = ytec::migrationcore::
      ShrinkSurplusAllocation::selected_data_partition;
  data.request.surplus_target_source_table_index = 5U;
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The targeted-surplus review must succeed");
  const auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(plan.has_value(),
        "A reviewed empty NTFS data partition may own the surplus extent");
  check(plan.value().target_plan().surplus_target_source_table_index() == 5U,
        "The reviewed source table index must remain the surplus owner");
}

void test_windows_boot_and_recovery_partitions_remain_forced_selected() {
  auto data = fixture();
  data.request.selected_source_table_indexes = {5U};
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The reduced UI selection review must succeed");
  const auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(plan.has_value(), "Required Windows partitions must be restored by planning");
  for (const std::uint32_t index : {1U, 2U, 3U, 4U}) {
    const auto& mapping = mapping_for_source(plan.value(), index);
    check(
        mapping.selected && mapping.required && mapping.target_number.has_value(),
        "Every Windows/ESP/MSR/Recovery mapping must remain selected and required");
  }
  const auto& optional_data = mapping_for_source(plan.value(), 5U);
  check(
      optional_data.selected && !optional_data.required,
      "The explicitly selected data partition must remain optional and selected");
}

Fixture exact_raw_fixture() {
  auto data = fixture();
  auto& raw = data.analysis.partitions.back();
  raw.file_system = ytec::migrationcore::MigrationFileSystem::unsupported;
  raw.used_bytes = raw.source_size_bytes;
  raw.cluster_size = 0U;
  raw.type_id = filled<16U>(0x9AU);
  data.analysis.content_volumes.pop_back();
  data.request.reviewed_target.size_bytes = 48ULL * kGiB;
  data.request.reviewed_target.sector_count =
      data.request.reviewed_target.size_bytes /
      data.request.reviewed_target.logical_sector_size;
  return data;
}

void test_selected_unsupported_data_is_retained_as_original_size_exact_raw() {
  auto data = exact_raw_fixture();
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The exact RAW review must succeed without volume binding");
  const auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(plan.has_value(), "A selected unsupported data partition must plan exact RAW");
  const auto& raw = task_for_source(plan.value(), 5U);
  check(
      raw.kind == ytec::directshrink::PartitionTaskKind::copy_exact_raw &&
          raw.construction_size_bytes == raw.source_size_bytes &&
          raw.target_size_bytes == raw.source_size_bytes &&
          raw.source_partition_type == filled<16U>(0x9AU) &&
          raw.original_volume_guid_path.empty(),
      "Unsupported data must retain exact source extent and GPT type without WIM");
}

void test_review_binding_rejects_changed_analysis() {
  auto data = fixture();
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The initial review must succeed");
  data.analysis.partitions.back().label = L"Changed after review";
  const auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(!plan.has_value(),
        "A changed source analysis must invalidate the completed review binding");
}

void test_invalid_review_identity_fails_without_unchecked_value_access() {
  auto data = fixture();
  data.request.reviewed_source.model.clear();
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(!review.has_value(),
        "An invalid reviewed identity must fail closed before hashing or planning");
}

void test_4kn_source_or_target_fails_closed_before_planning() {
  auto source_4kn = fixture();
  source_4kn.request.reviewed_source.logical_sector_size = 4096U;
  source_4kn.request.reviewed_source.sector_count =
      source_4kn.request.reviewed_source.size_bytes / 4096U;
  source_4kn.analysis.source.logical_sector_size = 4096U;
  const auto source_review = ytec::winpeapp::
      inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          source_4kn.request, source_4kn.analysis);
  check(
      !source_review.has_value() &&
          source_review.error().code ==
              ytec::clonecore::ErrorCode::unsupported_layout,
      "A 4Kn source must fail closed before a partition review is exposed");

  auto target_4kn = fixture();
  target_4kn.request.reviewed_target.logical_sector_size = 4096U;
  target_4kn.request.reviewed_target.sector_count =
      target_4kn.request.reviewed_target.size_bytes / 4096U;
  const auto target_review = ytec::winpeapp::
      inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          target_4kn.request, target_4kn.analysis);
  check(
      !target_review.has_value() &&
          target_review.error().code ==
              ytec::clonecore::ErrorCode::unsupported_layout,
      "A 4Kn target must fail closed before a partition review is exposed");
}

void test_no_io_preflight_rejects_unsafe_environment_before_source_latch() {
  auto data = fixture();
  check(
      ytec::winpeapp::
          validate_winpe_offline_ntfs_direct_shrink_no_io_preflight(
              data.request)
          .has_value(),
      "The baseline product request must pass the pure no-I/O preflight");

  data.request.administrator = false;
  check(
      !ytec::winpeapp::
           validate_winpe_offline_ntfs_direct_shrink_no_io_preflight(
               data.request)
           .has_value(),
      "A non-administrator request must stop at the no-I/O preflight");

  std::ifstream input(
      YTEC_WINPE_DIRECT_SHRINK_PRODUCT_SOURCE_PATH,
      std::ios::binary);
  check(input.is_open(), "The product direct-shrink adapter source must be readable");
  const std::string source{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()};
  const auto observer = source.find(
      "clonecore::Result<ProductObservation> observe_product_with_windows_apis");
  const auto preflight = source.find(
      "validate_winpe_offline_ntfs_direct_shrink_no_io_preflight(request)",
      observer);
  const auto source_latch = source.find(
      "set_verified_source_read_only_with_windows_apis", preflight);
  check(
      observer != std::string::npos && preflight != std::string::npos &&
          source_latch != std::string::npos && observer < preflight &&
          preflight < source_latch,
      "The pure preflight must run before the product source read-only latch");
}

template <typename T>
ytec::clonecore::Result<T> synthetic_failure(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Result<T>::failure(ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = std::move(message),
  });
}

struct ExecutionState final {
  std::uint64_t capture_count{};
  std::uint64_t apply_count{};
  std::uint64_t raw_copy_count{};
  std::uint64_t discard_count{};
  std::uint64_t commit_count{};
  std::uint64_t abort_count{};
  bool fail_first_capture{};
  bool fail_raw_copy{};
  bool abort_kept_target_offline{};
};

class SyntheticRawSourceReader final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit SyntheticRawSourceReader(const std::uint64_t size_bytes) noexcept
      : size_bytes_(size_bytes) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_bytes_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > size_bytes_ || length > size_bytes_ - offset) {
      return synthetic_failure<std::vector<std::byte>>(
          L"Synthetic exact RAW source", L"Requested range is out of bounds");
    }
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0U; index < length; ++index) {
      bytes[index] = static_cast<std::byte>((offset + index + 0x9AU) & 0xFFU);
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

 private:
  std::uint64_t size_bytes_{};
};

class SyntheticCaptureLease final
    : public ytec::winpeapp::IWinPeOfflineNtfsCaptureLease {
 public:
  explicit SyntheticCaptureLease(
      ytec::winpeapp::WinPeOfflineNtfsCaptureLeaseEvidence evidence)
      : evidence_(std::move(evidence)) {}

  [[nodiscard]] const ytec::winpeapp::
      WinPeOfflineNtfsCaptureLeaseEvidence&
  evidence() const noexcept override {
    return evidence_;
  }

 private:
  ytec::winpeapp::WinPeOfflineNtfsCaptureLeaseEvidence evidence_;
};

class SyntheticSourceGuard final
    : public ytec::winpeapp::IWinPeOfflineNtfsSourceGuard {
 public:
  explicit SyntheticSourceGuard(
      ytec::winpeapp::WinPeOfflineNtfsSourceEpochEvidence epoch)
      : epoch_(std::move(epoch)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::winpeapp::WinPeOfflineNtfsSourceEpochEvidence>
  lock_source_read_only_and_revalidate(
      const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan&) override {
    locked_ = true;
    return ytec::clonecore::Result<
        ytec::winpeapp::WinPeOfflineNtfsSourceEpochEvidence>::success(epoch_);
  }

  [[nodiscard]] ytec::clonecore::Result<std::unique_ptr<
      ytec::winpeapp::IWinPeOfflineNtfsCaptureLease>>
  acquire_capture_lease_after_revalidation(
      const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan&,
      const ytec::directshrink::PartitionTask& task) override {
    const auto found = std::find_if(
        epoch_.ntfs_volumes.begin(),
        epoch_.ntfs_volumes.end(),
        [&task](const auto& volume) {
          return task.source_table_index.has_value() &&
              volume.source_table_index == *task.source_table_index;
        });
    if (found == epoch_.ntfs_volumes.end()) {
      return synthetic_failure<std::unique_ptr<
          ytec::winpeapp::IWinPeOfflineNtfsCaptureLease>>(
          L"Synthetic WinPE lease",
          L"The requested source extent has no retained volume binding");
    }
    ytec::winpeapp::WinPeOfflineNtfsCaptureLeaseEvidence evidence{
        .source_epoch = epoch_,
        .volume = *found,
        .capture_device_path = found->volume_guid_path,
        .capture_identity =
            L"SYNTHETIC-READ-ONLY-LEASE-" +
            std::to_wstring(found->source_table_index),
        .source_disk_handle_held_read_only = true,
        .source_volume_handle_held_read_only = true,
        .source_volume_extent_reverified = true,
    };
    std::unique_ptr<ytec::winpeapp::IWinPeOfflineNtfsCaptureLease> lease =
        std::make_unique<SyntheticCaptureLease>(std::move(evidence));
    return ytec::clonecore::Result<std::unique_ptr<
        ytec::winpeapp::IWinPeOfflineNtfsCaptureLease>>::success(
        std::move(lease));
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::winpeapp::WinPeOfflineNtfsSourceEpochEvidence>
  revalidate_immediately_before_final_commit(
      const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan&) override {
    return ytec::clonecore::Result<
        ytec::winpeapp::WinPeOfflineNtfsSourceEpochEvidence>::success(epoch_);
  }

  [[nodiscard]] bool source_left_os_read_only() const noexcept override {
    return locked_;
  }

 private:
  ytec::winpeapp::WinPeOfflineNtfsSourceEpochEvidence epoch_;
  bool locked_{};
};

class SyntheticTargetPlatform final
    : public ytec::directshrink::ITargetPlatform {
 public:
  explicit SyntheticTargetPlatform(std::shared_ptr<ExecutionState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  begin_target_owned_staging(
      const ytec::directshrink::TargetPlan& plan,
      const ytec::operationcore::Sha256Digest& operation_plan_hash) override {
    plan_ = std::addressof(plan);
    operation_plan_hash_ = operation_plan_hash;
    return ytec::clonecore::Result<
        ytec::directshrink::CheckpointEvidence>::success(
        checkpoint(
            ytec::directshrink::CheckpointPhase::prepared,
            1U,
            0U,
            0U,
            {}));
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::TargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      const std::span<const ytec::directshrink::PartitionTask> tasks)
      override {
    const auto count = static_cast<std::uint64_t>(std::count_if(
        tasks.begin(),
        tasks.end(),
        [](const auto& task) {
          return task.kind !=
                  ytec::directshrink::PartitionTaskKind::apply_ntfs_wim &&
              task.kind !=
                  ytec::directshrink::PartitionTaskKind::copy_exact_raw;
        }));
    return ytec::clonecore::Result<
        ytec::directshrink::TargetPreparationEvidence>::success({
        .prepared_task_count = count,
        .verified_target_bytes = count == 0U ? 0U : 1024U,
        .write_digest = count == 0U ? ytec::imageformat::Sha256Digest{}
                                    : filled<32U>(0x31U),
        .every_write_flushed = true,
        .every_write_read_back = true,
        .target_offline = true,
        .final_layout_committed = false,
    });
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::StagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const ytec::directshrink::PartitionTask& task,
      const ytec::directshrink::CaptureSource& source) override {
    if (state_->fail_first_capture && state_->capture_count == 0U) {
      return synthetic_failure<ytec::directshrink::StagedArchiveEvidence>(
          L"Synthetic WinPE WIM capture",
          L"Injected pre-publication capture failure");
    }
    ++state_->capture_count;
    return ytec::clonecore::Result<
        ytec::directshrink::StagedArchiveEvidence>::success({
        .source_table_index = source.source_table_index,
        .target_number = task.target_number,
        .capture_identity = source.capture_identity,
        .read_device_path = source.read_device_path,
        .archive_length = 4096U,
        .archive_hash = filled<32U>(
            static_cast<unsigned char>(0x40U + task.target_number)),
        .sealed_no_write_delete_sharing = true,
        .flushed = true,
        .complete_read_back_hash_verified = true,
        .target_offline = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::AppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const ytec::directshrink::PartitionTask& task,
      const ytec::directshrink::StagedArchiveEvidence& archive) override {
    ++state_->apply_count;
    return ytec::clonecore::Result<
        ytec::directshrink::AppliedPartitionEvidence>::success({
        .source_table_index = archive.source_table_index,
        .target_number = task.target_number,
        .verified_target_bytes = 4096U,
        .archive_hash = archive.archive_hash,
        .target_write_digest = filled<32U>(
            static_cast<unsigned char>(0x60U + task.target_number)),
        .every_write_flushed = true,
        .every_write_read_back = true,
        .file_system_metadata_verified = true,
        .target_offline = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::ExactRawPartitionEvidence>
  copy_exact_raw_and_verify(
      const ytec::directshrink::PartitionTask& task,
      const ytec::clonecore::ISourceDiskReader& read_only_source) override {
    if (state_->fail_raw_copy) {
      return synthetic_failure<
          ytec::directshrink::ExactRawPartitionEvidence>(
          L"Synthetic exact RAW copy",
          L"Injected exact RAW verification failure");
    }
    check(
        task.source_table_index.has_value() &&
            read_only_source.size_bytes() == plan_->expected_source().size_bytes &&
            read_only_source.logical_sector_size() == 512U,
        "The synthetic RAW adapter must receive the exact reviewed source");
    ++state_->raw_copy_count;
    const auto digest = filled<32U>(0x91U);
    return ytec::clonecore::Result<
        ytec::directshrink::ExactRawPartitionEvidence>::success({
        .source_table_index = *task.source_table_index,
        .target_number = task.target_number,
        .verified_target_bytes = task.source_size_bytes,
        .verified_chunk_count = 1U,
        .source_sha256 = digest,
        .target_sha256 = digest,
        .target_write_digest = digest,
        .source_reader_read_only = true,
        .source_extent_exact = true,
        .every_write_flushed = true,
        .every_chunk_read_back = true,
        .complete_target_hash_verified = true,
        .target_offline = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status discard_exact_staged_archive(
      const ytec::directshrink::StagedArchiveEvidence&) override {
    ++state_->discard_count;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const ytec::directshrink::CheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest)
      override {
    return advanced_checkpoint(
        previous,
        ytec::directshrink::CheckpointPhase::applying,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  persist_progress_checkpoint(
      const ytec::directshrink::CheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest)
      override {
    return advanced_checkpoint(
        previous,
        ytec::directshrink::CheckpointPhase::applying,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  [[nodiscard]] ytec::clonecore::Result<ytec::directshrink::BootEvidence>
  finalize_boot_from_staged_layout_and_verify(
      const ytec::directshrink::TargetPlan& plan) override {
    return ytec::clonecore::Result<
        ytec::directshrink::BootEvidence>::success({
        .required = plan.boot_finalization_required(),
        .completed = true,
        .boot_files_read_back_verified = true,
        .recovery_configuration_verified = true,
        .target_offline = true,
        .target_only_reconstruction = true,
        .exact_target_volume_extents = true,
        .legacy_bios = false,
        .real_boot_not_claimed = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  seal_commit_ready_checkpoint(
      const ytec::directshrink::CheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest)
      override {
    return advanced_checkpoint(
        previous,
        ytec::directshrink::CheckpointPhase::commit_ready,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  prepare_final_extents_keep_incomplete_and_verify(
      const ytec::directshrink::TargetPlan&,
      const ytec::directshrink::CheckpointEvidence& expected) override {
    return ytec::clonecore::Result<
        ytec::directshrink::CheckpointEvidence>::success(expected);
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  revalidate_before_final_commit(
      const ytec::directshrink::TargetPlan&,
      const ytec::directshrink::CheckpointEvidence& expected) override {
    return ytec::clonecore::Result<
        ytec::directshrink::CheckpointEvidence>::success(expected);
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::FinalCommitEvidence>
  commit_final_layout_last(
      const ytec::directshrink::TargetPlan& plan,
      const ytec::directshrink::CheckpointEvidence& commit_ready) override {
    ++state_->commit_count;
    return ytec::clonecore::Result<
        ytec::directshrink::FinalCommitEvidence>::success({
        .committed_layout_hash = plan.final_layout_hash(),
        .aggregate_write_digest = commit_ready.aggregate_write_digest,
        .source_reidentified = true,
        .source_layout_unchanged = true,
        .target_reidentified = true,
        .staging_identity_reverified = true,
        .checkpoint_reverified = true,
        .staging_removed = true,
        .checkpoint_retired = true,
        .checkpoint_retirement_pending = false,
        .construction_layout_non_bootable = true,
        .checkpoint_retained_through_extensions_and_boot = true,
        .boot_completed_before_final_layout_publication = true,
        .final_layout_published_before_checkpoint_retirement = true,
        .hidden_final_layout_published_and_read_back = true,
        .extended_ntfs_partition_count = plan.ntfs_extension_task_count(),
        .every_required_ntfs_extension_verified = true,
        .every_write_flushed = true,
        .every_write_read_back = true,
        .primary_layout_committed_last = true,
        .target_offline = true,
        .final_partition_style =
            ytec::migrationcore::MigrationPartitionStyle::gpt,
    });
  }

  void abort_keep_offline_incomplete() noexcept override {
    ++state_->abort_count;
    state_->abort_kept_target_offline = true;
  }

 private:
  [[nodiscard]] ytec::directshrink::CheckpointEvidence checkpoint(
      const ytec::directshrink::CheckpointPhase phase,
      const std::uint64_t revision,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest) const {
    return ytec::directshrink::CheckpointEvidence{
        .phase = phase,
        .revision = revision,
        .plan_hash = operation_plan_hash_,
        .staging_identity_hash = filled<32U>(0x21U),
        .record_hash = filled<32U>(
            static_cast<unsigned char>(0x70U + revision)),
        .aggregate_write_digest = aggregate_write_digest,
        .observed_target = plan_->expected_target(),
        .completed_task_count = completed_task_count,
        .verified_target_bytes = verified_target_bytes,
        .durable = true,
        .flushed = true,
        .read_back_verified = true,
        .target_offline = true,
        .final_layout_committed = false,
    };
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::directshrink::CheckpointEvidence>
  advanced_checkpoint(
      const ytec::directshrink::CheckpointEvidence& previous,
      const ytec::directshrink::CheckpointPhase phase,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const ytec::imageformat::Sha256Digest& aggregate_write_digest) const {
    return ytec::clonecore::Result<
        ytec::directshrink::CheckpointEvidence>::success(checkpoint(
        phase,
        previous.revision + 1U,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest));
  }

  std::shared_ptr<ExecutionState> state_;
  const ytec::directshrink::TargetPlan* plan_{};
  ytec::operationcore::Sha256Digest operation_plan_hash_{};
};

ytec::diskmodel::ReidentifiedPhysicalClone observed_clone(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan& plan) {
  auto source = source_disk();
  source.size_bytes = plan.target_plan().expected_source().size_bytes;
  source.logical_sector_size =
      plan.target_plan().expected_source().logical_sector_size;
  source.sector_count = source.size_bytes / source.logical_sector_size;
  auto target = target_disk();
  target.size_bytes = plan.target_plan().expected_target().size_bytes;
  target.logical_sector_size =
      plan.target_plan().expected_target().logical_sector_size;
  target.sector_count = target.size_bytes / target.logical_sector_size;
  return ytec::diskmodel::ReidentifiedPhysicalClone{
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = plan.target_plan().expected_source(),
      .target_identity = plan.target_plan().expected_target(),
  };
}

ytec::winpeapp::WinPeOfflineNtfsDirectShrinkDependencies
synthetic_dependencies(
    const ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan& plan,
    std::shared_ptr<ExecutionState> state) {
  const auto observed = observed_clone(plan);
  const auto epoch = plan.planning_evidence().source_epoch;
  return ytec::winpeapp::WinPeOfflineNtfsDirectShrinkDependencies{
      .reidentify_selection =
          [observed](const auto&, const auto&) {
            return ytec::clonecore::Result<
                ytec::diskmodel::ReidentifiedPhysicalClone>::success(
                observed);
          },
      .reidentify_confirmed =
          [observed](const auto&, const auto&, const auto&) {
            return ytec::clonecore::Result<
                ytec::diskmodel::ReidentifiedPhysicalClone>::success(
                observed);
          },
      .make_source_guard =
          [epoch](const auto&, const auto&) {
            std::unique_ptr<
                ytec::winpeapp::IWinPeOfflineNtfsSourceGuard> guard =
                std::make_unique<SyntheticSourceGuard>(epoch);
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::winpeapp::IWinPeOfflineNtfsSourceGuard>>::success(
                std::move(guard));
          },
      .make_target_platform =
          [state = std::move(state)](const auto&, const auto&) {
            std::unique_ptr<ytec::directshrink::ITargetPlatform> target =
                std::make_unique<SyntheticTargetPlatform>(state);
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::directshrink::ITargetPlatform>>::success(
                std::move(target));
          },
      .open_read_only_raw_source =
          [observed](const auto& expected_source) {
            return ytec::clonecore::Result<
                ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success({
                .observed = {
                    .observed = observed.source,
                    .identity = expected_source,
                },
                .reader = std::make_unique<SyntheticRawSourceReader>(
                    expected_source.size_bytes),
            });
          },
  };
}

ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan planned_fixture() {
  auto data = fixture();
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The execution review fixture must succeed");
  auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(plan.has_value(), "The execution plan fixture must succeed");
  return plan.take_value();
}

ytec::winpeapp::WinPeOfflineNtfsDirectShrinkPlan planned_exact_raw_fixture() {
  auto data = exact_raw_fixture();
  const auto review =
      ytec::winpeapp::inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
          data.request, data.analysis);
  check(review.has_value(), "The exact RAW execution review fixture must succeed");
  auto plan = ytec::winpeapp::
      build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
          data.request, data.analysis, review.value().binding);
  check(plan.has_value(), "The exact RAW execution plan fixture must succeed");
  return plan.take_value();
}

void test_synthetic_controller_completes_the_full_commit_last_path() {
  auto plan = planned_fixture();
  auto state = std::make_shared<ExecutionState>();
  const auto dependencies = synthetic_dependencies(plan, state);
  const auto result =
      ytec::winpeapp::execute_winpe_offline_ntfs_direct_shrink_clone(
          plan,
          ytec::winpeapp::WinPeOfflineNtfsDirectShrinkExecutionOptions{
              .confirmation = {
                  .first_step_acknowledged = true,
                  .typed_token = L"OK",
              },
          },
          dependencies);
  check(result.has_value(), "The synthetic controller call must return a report");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          result.value().execution.has_value(),
      "The synthetic controller must complete and retain execution evidence");
  check(
      result.value().execution->source_epoch_revalidation_count == 4U &&
          result.value().execution->applied_archive_count == 2U &&
          state->capture_count == 2U && state->apply_count == 2U &&
          state->discard_count == 2U && state->commit_count == 1U &&
          state->abort_count == 0U,
      "The controller must revalidate every source epoch and commit exactly once");
}

void test_synthetic_controller_aborts_before_publication_on_capture_failure() {
  auto plan = planned_fixture();
  auto state = std::make_shared<ExecutionState>();
  state->fail_first_capture = true;
  const auto dependencies = synthetic_dependencies(plan, state);
  const auto result =
      ytec::winpeapp::execute_winpe_offline_ntfs_direct_shrink_clone(
          plan,
          ytec::winpeapp::WinPeOfflineNtfsDirectShrinkExecutionOptions{
              .confirmation = {
                  .first_step_acknowledged = true,
                  .typed_token = L"OK",
              },
          },
          dependencies);
  check(result.has_value(), "A controlled execution failure must return lifecycle evidence");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          !result.value().execution.has_value() &&
          state->capture_count == 0U && state->commit_count == 0U &&
          state->abort_count == 1U && state->abort_kept_target_offline,
      "A pre-publication capture failure must abort once and never commit");
}

void test_synthetic_controller_copies_exact_raw_before_commit_last() {
  auto plan = planned_exact_raw_fixture();
  auto state = std::make_shared<ExecutionState>();
  const auto dependencies = synthetic_dependencies(plan, state);
  const auto result =
      ytec::winpeapp::execute_winpe_offline_ntfs_direct_shrink_clone(
          plan,
          ytec::winpeapp::WinPeOfflineNtfsDirectShrinkExecutionOptions{
              .confirmation = {
                  .first_step_acknowledged = true,
                  .typed_token = L"OK",
              },
          },
          dependencies);
  check(result.has_value(), "The synthetic exact RAW controller call must return");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          result.value().execution.has_value() &&
          result.value().execution->copied_exact_raw_count == 1U &&
          result.value().execution->source_epoch_revalidation_count == 5U &&
          state->raw_copy_count == 1U && state->commit_count == 1U &&
          state->abort_count == 0U,
      "Exact RAW must be read-only reidentified, verified, and committed once last");
}

void test_synthetic_controller_aborts_and_keeps_offline_on_raw_failure() {
  auto plan = planned_exact_raw_fixture();
  auto state = std::make_shared<ExecutionState>();
  state->fail_raw_copy = true;
  const auto dependencies = synthetic_dependencies(plan, state);
  const auto result =
      ytec::winpeapp::execute_winpe_offline_ntfs_direct_shrink_clone(
          plan,
          ytec::winpeapp::WinPeOfflineNtfsDirectShrinkExecutionOptions{
              .confirmation = {
                  .first_step_acknowledged = true,
                  .typed_token = L"OK",
              },
          },
          dependencies);
  check(result.has_value(), "A controlled exact RAW failure must return lifecycle evidence");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          !result.value().execution.has_value() &&
          state->raw_copy_count == 0U && state->commit_count == 0U &&
          state->abort_count == 1U && state->abort_kept_target_offline,
      "An exact RAW verification failure must abort offline and never publish GPT");
}

void test_windows_product_dependencies_are_fully_wired_without_io() {
  const auto dependencies = ytec::winpeapp::
      make_winpe_offline_ntfs_direct_shrink_dependencies_with_windows_apis({
          .confirmation = {
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
      });
  check(
      static_cast<bool>(dependencies.reidentify_selection) &&
          static_cast<bool>(dependencies.reidentify_confirmed) &&
          static_cast<bool>(dependencies.make_source_guard) &&
          static_cast<bool>(dependencies.make_target_platform) &&
          static_cast<bool>(dependencies.open_read_only_raw_source),
      "The production factory must wire every controller dependency");
}

void test_winpe_gui_exposes_complete_direct_shrink_product_route() {
  std::ifstream input(
      YTEC_WINPE_GUI_SOURCE_PATH,
      std::ios::binary);
  check(input.is_open(), "The product WinPE GUI source must be readable");
  const std::string source{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()};
  constexpr std::array required_markers{
      std::string_view{"CloneRoute::shrink"},
      std::string_view{"kCloneShrinkInspectionCompleteMessage"},
      std::string_view{"reviewed_direct_shrink_clone_plan"},
      std::string_view{"show_direct_shrink_partition_review_dialog"},
      std::string_view{
          "inspect_winpe_offline_ntfs_direct_shrink_with_windows_apis"},
      std::string_view{
          "plan_winpe_offline_ntfs_direct_shrink_after_review_with_windows_apis"},
      std::string_view{
          "make_winpe_offline_ntfs_direct_shrink_dependencies_with_windows_apis"},
      std::string_view{"execute_winpe_offline_ntfs_direct_shrink_clone"},
      std::string_view{"winpe_offline_direct_shrink_completion_verified"},
      std::string_view{"current_environment_is_verified_winpe"},
      std::string_view{"selected_source_table_indexes"},
      std::string_view{"automatic_proportional"},
      std::string_view{"leave_unallocated"},
      std::string_view{"selected_data_partition"},
      std::string_view{"LVIF_PARAM"},
      std::string_view{"LVN_ITEMCHANGING"},
      std::string_view{"LVN_KEYDOWN"},
      std::string_view{"VK_SPACE"},
      std::string_view{"control_text(state.clone_token) != L\"OK\""},
      std::string_view{".typed_token = L\"OK\""},
      std::string_view{"source_locked_read_only_before_target_io"},
      std::string_view{"copy_exact_raw"},
      std::string_view{"copied_exact_raw_count"},
      std::string_view{"final_gpt_committed_last"},
      std::string_view{"target_left_offline"},
  };
  for (const auto marker : required_markers) {
    check(
        source.find(marker) != std::string::npos,
        "The product WinPE GUI route is missing marker: " +
            std::string(marker));
  }
  const auto inspect = source.find(
      "inspect_winpe_offline_ntfs_direct_shrink_with_windows_apis");
  const auto plan = source.find(
      "plan_winpe_offline_ntfs_direct_shrink_after_review_with_windows_apis");
  const auto dependencies = source.find(
      "make_winpe_offline_ntfs_direct_shrink_dependencies_with_windows_apis");
  const auto execute = source.find(
      "execute_winpe_offline_ntfs_direct_shrink_clone");
  check(
      inspect < plan && plan < dependencies && dependencies < execute,
      "The product GUI must route inspection, reviewed planning, production "
      "dependencies, and execution in that order");
  check(
      source.find(
          "縮小移行: 今回のPE画面では未接続") == std::string::npos,
      "The retired not-connected direct-shrink label must not return");
}

}  // namespace

int main() {
  try {
    test_review_and_reanalysis_build_source_bound_empty_ntfs();
    test_selected_empty_data_can_receive_surplus();
    test_windows_boot_and_recovery_partitions_remain_forced_selected();
    test_selected_unsupported_data_is_retained_as_original_size_exact_raw();
    test_review_binding_rejects_changed_analysis();
    test_invalid_review_identity_fails_without_unchecked_value_access();
    test_4kn_source_or_target_fails_closed_before_planning();
    test_no_io_preflight_rejects_unsafe_environment_before_source_latch();
    test_synthetic_controller_completes_the_full_commit_last_path();
    test_synthetic_controller_aborts_before_publication_on_capture_failure();
    test_synthetic_controller_copies_exact_raw_before_commit_last();
    test_synthetic_controller_aborts_and_keeps_offline_on_raw_failure();
    test_windows_product_dependencies_are_fully_wired_without_io();
    test_winpe_gui_exposes_complete_direct_shrink_product_route();
    std::cout << "WinPE offline NTFS direct shrink product tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

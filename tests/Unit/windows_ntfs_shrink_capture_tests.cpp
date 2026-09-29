#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/windows_ntfs_shrink_capture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kDiskBytes = 64ULL * kMiB;
constexpr std::uint64_t kRawOffset = 20ULL * kMiB;
constexpr std::uint64_t kRawBytes = 4096U;
constexpr wchar_t kVolume1[] =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555551}\\";
constexpr wchar_t kVolume2[] =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555552}\\";
constexpr wchar_t kSnapshot1[] =
    L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy201";
constexpr wchar_t kSnapshot2[] =
    L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy202";
constexpr wchar_t kScratch[] = L"\\\\?\\D:\\YTEC\\scratch";

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error injected_error(const std::wstring_view operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::wstring(operation),
      .message = L"合成失敗です",
  };
}

ytec::clonecore::StableDiskIdentity source_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 1U,
      .model = L"Synthetic NTFS source",
      .size_bytes = kDiskBytes,
      .logical_sector_size = 512U,
      .serial_suffix = "SRC-N1",
      .device_instance_id = L"SYNTHETIC\\NTFS-SOURCE",
      .is_system_disk = false,
  };
}

std::array<std::byte, 512U> mbr_sector() {
  std::array<std::byte, 512U> result{};
  result[510U] = std::byte{0x55};
  result[511U] = std::byte{0xAA};
  return result;
}

template <typename T>
void write_little(
    std::array<std::byte, 512U>& bytes,
    const std::size_t offset,
    T value) {
  static_assert(std::is_unsigned_v<T>);
  for (std::size_t index = 0U; index < sizeof(T); ++index) {
    bytes[offset + index] = static_cast<std::byte>(value & 0xFFU);
    value >>= 8U;
  }
}

std::array<std::byte, 512U> ntfs_boot_sector() {
  std::array<std::byte, 512U> result{};
  constexpr std::string_view signature = "NTFS    ";
  for (std::size_t index = 0U; index < signature.size(); ++index) {
    result[3U + index] = static_cast<std::byte>(signature[index]);
  }
  write_little<std::uint16_t>(result, 11U, 512U);
  result[13U] = std::byte{8};
  write_little<std::uint64_t>(result, 40U, (8ULL * kMiB) / 512U);
  result[510U] = std::byte{0x55};
  result[511U] = std::byte{0xAA};
  return result;
}

std::vector<std::byte> partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  const auto mbr = mbr_sector();
  snapshot.regions.push_back({
      .disk_offset = 0U,
      .data = std::vector<std::byte>(mbr.begin(), mbr.end()),
  });
  auto built = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(built.has_value(), "partition snapshot fixture should build");
  return built.take_value();
}

ytec::imageformat::TsumugiManifestPartition ntfs_partition(
    const std::uint32_t index,
    const std::uint64_t offset) {
  using namespace ytec::imageformat;
  TsumugiManifestPartition result{
      .source_table_index = index,
      .source_partition_number = index,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected,
      .source_offset = offset,
      .source_size = 8ULL * kMiB,
      .used_bytes = 2ULL * kMiB,
      .minimum_target_bytes = 4ULL * kMiB,
      .planned_target_bytes = 4ULL * kMiB,
      .cluster_size = 4096U,
      .name_utf8 = "NTFS",
      .label_utf8 = "NTFS",
  };
  result.type_id[0] = std::byte{0x07};
  return result;
}

ytec::imageformat::TsumugiManifestPartition raw_partition() {
  using namespace ytec::imageformat;
  TsumugiManifestPartition result{
      .source_table_index = 3U,
      .source_partition_number = 3U,
      .role = TsumugiManifestPartitionRole::other,
      .file_system = TsumugiManifestFileSystem::none,
      .flags = TsumugiManifestPartitionFlags::selected,
      .source_offset = kRawOffset,
      .source_size = kRawBytes,
      .used_bytes = kRawBytes,
      .minimum_target_bytes = kRawBytes,
      .planned_target_bytes = kRawBytes,
      .name_utf8 = "Static",
      .label_utf8 = "Static",
  };
  result.type_id[0] = std::byte{0x27};
  return result;
}

ytec::windowsapp::WindowsNtfsShrinkCapturePlan capture_plan() {
  using namespace ytec;
  const auto identity = source_identity();
  auto model_hash = imageformat::hash_tsumugi_source_model_v1(identity.model);
  auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
      identity.serial_suffix, identity.device_instance_id);
  check(model_hash.has_value() && serial_hash.has_value(),
        "source hash fixtures should build");
  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::shrink,
      .partition_style =
          imageformat::TsumugiManifestPartitionStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .source_model_hash = model_hash.value(),
      .source_serial_hash = serial_hash.value(),
      .created_utc = "2026-08-09T03:00:00Z",
      .app_version = "1.0.0",
      .partitions = {
          ntfs_partition(1U, 1ULL * kMiB),
          ntfs_partition(2U, 10ULL * kMiB),
          raw_partition(),
      },
      .partition_snapshot = partition_snapshot(),
  };
  return windowsapp::WindowsNtfsShrinkCapturePlan{
      .source_disk = identity,
      .reviewed_manifest = std::move(manifest),
      .snapshot_volumes = {
          {.source_table_index = 1U,
           .original_volume_guid_path = kVolume1},
          {.source_table_index = 2U,
           .original_volume_guid_path = kVolume2},
      },
  };
}

struct ReaderState final {
  std::uint8_t raw_generation{};
  bool expose_ntfs_boot{true};
};

class SyntheticDiskReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit SyntheticDiskReader(std::shared_ptr<ReaderState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > kDiskBytes || length > kDiskBytes - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"synthetic source range"));
    }
    const auto mbr = mbr_sector();
    const auto ntfs = ntfs_boot_sector();
    std::vector<std::byte> result(length);
    for (std::size_t index = 0U; index < length; ++index) {
      const std::uint64_t absolute = offset + index;
      if (absolute < mbr.size()) {
        result[index] = mbr[static_cast<std::size_t>(absolute)];
      } else if (state_->expose_ntfs_boot &&
                 absolute >= 1ULL * kMiB &&
                 absolute < 1ULL * kMiB + ntfs.size()) {
        result[index] = ntfs[static_cast<std::size_t>(
            absolute - 1ULL * kMiB)];
      } else if (state_->expose_ntfs_boot &&
                 absolute >= 10ULL * kMiB &&
                 absolute < 10ULL * kMiB + ntfs.size()) {
        result[index] = ntfs[static_cast<std::size_t>(
            absolute - 10ULL * kMiB)];
      } else if (absolute >= kRawOffset &&
                 absolute < kRawOffset + kRawBytes) {
        result[index] = static_cast<std::byte>(
            ((absolute - kRawOffset) * 19U + state_->raw_generation) &
            0xFFU);
      } else {
        result[index] = static_cast<std::byte>((absolute * 7U + 3U) & 0xFFU);
      }
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

 private:
  std::shared_ptr<ReaderState> state_;
};

struct FakeState final {
  std::shared_ptr<ReaderState> reader{std::make_shared<ReaderState>()};
  std::size_t source_open_calls{};
  std::size_t volume_resolver_calls{};
  std::size_t staging_factory_calls{};
  std::size_t dism_calls{};
  std::size_t discard_calls{};
  bool observed_extent_mismatch{};
  bool unsigned_dism{};
  bool discarded{};
  std::map<std::wstring, std::uint32_t> path_to_table;
  std::map<std::uint32_t, std::vector<std::byte>> wims;
  std::vector<ytec::windowsdism::DismCaptureRequest> dism_requests;
};

ytec::diskmodel::DiskInfo disk_info(const FakeState& state) {
  using namespace ytec::diskmodel;
  DiskInfo disk{
      .disk_number = 1U,
      .device_path = L"\\\\.\\PhysicalDrive1",
      .device_instance_id = source_identity().device_instance_id,
      .model = source_identity().model,
      .size_bytes = kDiskBytes,
      .sector_count = kDiskBytes / 512U,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"Synthetic",
      .serial_suffix = source_identity().serial_suffix,
      .partition_style = PartitionStyle::mbr,
      .is_system_disk = false,
      .partitions = {
          {.number = 1U,
           .offset_bytes = 1ULL * kMiB,
           .size_bytes = 8ULL * kMiB,
           .style = PartitionStyle::mbr},
          {.number = 2U,
           .offset_bytes = 10ULL * kMiB,
           .size_bytes = 8ULL * kMiB,
           .style = PartitionStyle::mbr},
          {.number = 3U,
           .offset_bytes = kRawOffset,
           .size_bytes = kRawBytes,
           .style = PartitionStyle::mbr},
      },
  };
  if (state.observed_extent_mismatch) {
    disk.partitions[1].size_bytes -= 512U;
  }
  return disk;
}

class FakeStaging final
    : public ytec::windowsapp::IWindowsNtfsShrinkWimStaging {
 public:
  FakeStaging(FakeState& state, std::wstring scratch)
      : state_(state), scratch_(std::move(scratch)) {}

  [[nodiscard]] ytec::clonecore::Result<std::wstring> reserve_wim_path(
      const std::uint32_t table_index) override {
    const std::wstring path = scratch_ + L"\\owned\\volume-" +
        std::to_wstring(table_index) + L".wim";
    state_.path_to_table.emplace(path, table_index);
    return ytec::clonecore::Result<std::wstring>::success(path);
  }

  [[nodiscard]] ytec::clonecore::Result<std::uint64_t> seal_wim(
      const std::uint32_t table_index) override {
    const auto found = state_.wims.find(table_index);
    if (found == state_.wims.end()) {
      return ytec::clonecore::Result<std::uint64_t>::failure(
          injected_error(L"fake seal"));
    }
    return ytec::clonecore::Result<std::uint64_t>::success(
        found->second.size());
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_wim(
      const std::uint32_t table_index,
      const std::uint64_t offset,
      const std::size_t length) const override {
    const auto found = state_.wims.find(table_index);
    if (found == state_.wims.end() || offset > found->second.size() ||
        length > found->second.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"fake WIM read"));
    }
    const auto first = found->second.begin() +
        static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

  [[nodiscard]] ytec::clonecore::Status discard_owned() noexcept override {
    ++state_.discard_calls;
    state_.discarded = true;
    state_.wims.clear();
    return ytec::clonecore::success_status();
  }

 private:
  FakeState& state_;
  std::wstring scratch_;
};

ytec::windowsapp::WindowsNtfsShrinkCaptureDependencies dependencies(
    FakeState& state) {
  using namespace ytec;
  return {
      .open_read_only_source =
          [&state](const clonecore::StableDiskIdentity&) {
            ++state.source_open_calls;
            std::unique_ptr<clonecore::ISourceDiskReader> reader =
                std::make_unique<SyntheticDiskReader>(state.reader);
            return clonecore::Result<
                diskmodel::ReadOnlyPhysicalDiskHandle>::success({
                .observed = {
                    .observed = disk_info(state),
                    .identity = source_identity(),
                },
                .reader = std::move(reader),
            });
          },
      .resolve_snapshot_volumes =
          [&state](
              const diskmodel::DiskInfo&,
              const std::span<const diskmodel::VolumePartitionLocation>
                  locations) {
            ++state.volume_resolver_calls;
            std::vector<clonecore::VolumeBitmapBinding> result;
            for (const auto& location : locations) {
              result.push_back({
                  .partition_entry_index = location.table_index,
                  .volume_device_path = location.table_index == 1U
                      ? kVolume1
                      : kVolume2,
              });
            }
            return clonecore::Result<std::vector<
                clonecore::VolumeBitmapBinding>>::success(std::move(result));
          },
      .create_wim_staging =
          [&state](const std::wstring& scratch) {
            ++state.staging_factory_calls;
            std::unique_ptr<windowsapp::IWindowsNtfsShrinkWimStaging> value =
                std::make_unique<FakeStaging>(state, scratch);
            return clonecore::Result<std::unique_ptr<
                windowsapp::IWindowsNtfsShrinkWimStaging>>::success(
                std::move(value));
          },
      .capture_wim =
          [&state](const windowsdism::DismCaptureRequest& request) {
            ++state.dism_calls;
            state.dism_requests.push_back(request);
            const auto table = state.path_to_table.find(request.image_path);
            if (table == state.path_to_table.end()) {
              return clonecore::Result<
                  windowsdism::DismExecutionReport>::failure(
                  injected_error(L"fake DISM path"));
            }
            std::vector<std::byte> bytes(
                1001U + static_cast<std::size_t>(table->second),
                static_cast<std::byte>(table->second));
            constexpr std::array<std::byte, 8U> signature{
                static_cast<std::byte>('M'), static_cast<std::byte>('S'),
                static_cast<std::byte>('W'), static_cast<std::byte>('I'),
                static_cast<std::byte>('M'), std::byte{0}, std::byte{0},
                std::byte{0},
            };
            std::copy(signature.begin(), signature.end(), bytes.begin());
            state.wims[table->second] = std::move(bytes);
            return clonecore::Result<
                windowsdism::DismExecutionReport>::success({
                .executable_path = L"C:\\Windows\\System32\\dism.exe",
                .exit_code = 0U,
                .microsoft_signature_verified = !state.unsigned_dism,
            });
          },
  };
}

ytec::vssrequester::SnapshotCopyContext snapshot_context(
    const bool extra_mapping = false) {
  ytec::vssrequester::SnapshotCopyContext result{
      .snapshot_set_id = L"snapshot-set-200",
      .mappings = {
          {.original_volume_guid_path = kVolume1,
           .snapshot_id = L"snapshot-201",
           .snapshot_device_path = kSnapshot1},
          {.original_volume_guid_path = kVolume2,
           .snapshot_id = L"snapshot-202",
           .snapshot_device_path = kSnapshot2},
      },
  };
  if (extra_mapping) {
    result.mappings.push_back({
        .original_volume_guid_path =
            L"\\\\?\\Volume{11111111-2222-3333-4444-555555555553}\\",
        .snapshot_id = L"snapshot-203",
        .snapshot_device_path =
            L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy203",
    });
  }
  return result;
}

ytec::windowsapp::WindowsShrinkWorkPaths work_paths() {
  return {
      .scratch_directory = kScratch,
      .checkpoint_path = L"\\\\?\\D:\\YTEC\\checkpoint.bin",
      .log_path = L"\\\\?\\D:\\YTEC\\operation.log",
  };
}

void test_two_ntfs_volumes_use_only_one_snapshot_set_and_snapshot_paths() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state;
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "valid NTFS capture plan should build");
  check(state.source_open_calls == 0U && state.dism_calls == 0U,
        "factory validation must perform no I/O");

  auto captured = built.value()(
      snapshot_context(), partitions, work_paths(), {});
  check(captured.has_value() && captured.value() != nullptr,
        "two-volume synthetic capture should succeed");
  check(state.source_open_calls == 1U &&
            state.volume_resolver_calls == 1U &&
            state.staging_factory_calls == 1U && state.dism_calls == 2U,
        "capture should open one source and run DISM once per NTFS volume");
  check(state.dism_requests.size() == 2U &&
            state.dism_requests[0].source_root ==
                std::wstring(kSnapshot1) + L"\\" &&
            state.dism_requests[1].source_root ==
                std::wstring(kSnapshot2) + L"\\" &&
            state.dism_requests[0].source_root != kVolume1 &&
            state.dism_requests[1].source_root != kVolume2 &&
            state.dism_requests[0].scratch_directory == kScratch &&
            state.dism_requests[1].scratch_directory == kScratch,
        "DISM source must be only the active Snapshot device path");

  auto session = captured.take_value();
  const auto payloads = session->payloads();
  check(payloads.size() == 3U &&
            payloads[0].kind == ytec::windowsapp::
                WindowsShrinkCapturedPayloadKind::vss_snapshot_wim &&
            payloads[1].kind == ytec::windowsapp::
                WindowsShrinkCapturedPayloadKind::vss_snapshot_wim &&
            payloads[2].kind == ytec::windowsapp::
                WindowsShrinkCapturedPayloadKind::
                    locked_read_only_exact_raw &&
            payloads[1].session_source_offset % 512U != 0U &&
            payloads[2].session_source_offset % 512U == 0U,
        "WIM offsets may be byte-aligned while exact RAW remains sector-aligned");
  const auto wim_bytes = session->read(
      payloads[1].session_source_offset, 8U);
  const auto raw_bytes = session->read(
      payloads[2].session_source_offset, 512U);
  const auto source_state_hash = session->source_state_hash();
  check(wim_bytes.has_value() && raw_bytes.has_value() &&
            !std::all_of(
                source_state_hash.begin(),
                source_state_hash.end(),
                [](const std::byte value) { return value == std::byte{0}; }),
        "session must expose immutable WIM/RAW bytes and a bound state hash");
  const auto discarded = session->discard_owned_staging();
  check(discarded.has_value() && state.source_open_calls == 2U &&
            state.volume_resolver_calls == 2U && state.discard_calls == 1U &&
            state.discarded,
        "cleanup must freshly re-identify source/layout before owned WIM removal");
}

void test_unsupported_filesystem_and_bitlocker_fail_before_dependency_io() {
  FakeState state;
  auto exfat = capture_plan();
  exfat.reviewed_manifest.partitions[0].file_system =
      ytec::imageformat::TsumugiManifestFileSystem::exfat;
  const auto exfat_result =
      ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
          std::move(exfat), dependencies(state));
  check(!exfat_result.has_value(), "selected exFAT must fail closed");

  auto bitlocker = capture_plan();
  bitlocker.reviewed_manifest.flags =
      ytec::imageformat::TsumugiManifestFlags::
          bitlocker_source_was_unlocked;
  const auto bitlocker_result =
      ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
          std::move(bitlocker), dependencies(state));
  check(!bitlocker_result.has_value(), "BitLocker marker must fail closed");
  check(state.source_open_calls == 0U &&
            state.volume_resolver_calls == 0U &&
            state.staging_factory_calls == 0U && state.dism_calls == 0U,
        "unsupported preflight must invoke no Platform I/O dependency");
}

void test_snapshot_count_mismatch_fails_before_source_open() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state;
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "valid plan should build");
  const auto captured = built.value()(
      snapshot_context(true), partitions, work_paths(), {});
  check(!captured.has_value(), "extra Snapshot mapping must fail closed");
  check(state.source_open_calls == 0U &&
            state.staging_factory_calls == 0U && state.dism_calls == 0U,
        "Snapshot binding mismatch must stop before source or DISM I/O");
}

void test_cancellation_stops_before_first_platform_io() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state;
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "valid plan should build");
  ytec::clonecore::DiskOperationCallbacks callbacks{
      .cancellation_requested = [] { return true; },
  };
  const auto captured = built.value()(
      snapshot_context(), partitions, work_paths(), callbacks);
  check(!captured.has_value(), "pre-I/O cancellation must stop capture");
  check(state.source_open_calls == 0U &&
            state.staging_factory_calls == 0U && state.dism_calls == 0U,
        "pre-I/O cancellation must call no Platform dependency");
}

void test_observed_extent_mismatch_stops_before_staging_and_dism() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state{.observed_extent_mismatch = true};
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "valid plan should build");
  const auto captured = built.value()(
      snapshot_context(), partitions, work_paths(), {});
  check(!captured.has_value(), "changed partition extent must fail closed");
  check(state.source_open_calls == 1U &&
            state.volume_resolver_calls == 0U &&
            state.staging_factory_calls == 0U && state.dism_calls == 0U,
        "extent mismatch must stop before staging and DISM");
}

void test_non_ntfs_or_bitlocker_boot_sector_stops_before_staging() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state;
  state.reader->expose_ntfs_boot = false;
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "reviewed NTFS plan should build");
  const auto captured = built.value()(
      snapshot_context(), partitions, work_paths(), {});
  check(!captured.has_value(),
        "non-NTFS/BitLocker physical boot sector must fail closed");
  check(state.source_open_calls == 1U &&
            state.volume_resolver_calls == 0U &&
            state.staging_factory_calls == 0U && state.dism_calls == 0U,
        "filesystem reality check must stop before staging and DISM");
}

void test_static_raw_change_blocks_cleanup_success_but_discards_owned_wims() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state;
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "valid plan should build");
  auto captured = built.value()(
      snapshot_context(), partitions, work_paths(), {});
  check(captured.has_value(), "capture should succeed before mutation");
  state.reader->raw_generation = 1U;
  const auto discarded = captured.value()->discard_owned_staging();
  check(!discarded.has_value() && state.source_open_calls == 2U &&
            state.discard_calls == 1U && state.discarded,
        "changed static RAW must fail final epoch while owned WIM is removed");
}

void test_unsigned_dism_report_is_rejected_and_staging_is_discarded() {
  auto plan = capture_plan();
  const auto partitions = plan.reviewed_manifest.partitions;
  FakeState state{.unsigned_dism = true};
  auto built = ytec::windowsapp::make_windows_ntfs_shrink_capture_executor(
      std::move(plan), dependencies(state));
  check(built.has_value(), "valid plan should build");
  const auto captured = built.value()(
      snapshot_context(), partitions, work_paths(), {});
  check(!captured.has_value() && state.dism_calls == 1U &&
            state.discard_calls == 1U && state.discarded,
        "untrusted DISM evidence must fail and remove only owned staging");
}

}  // namespace

int main() {
  try {
    test_two_ntfs_volumes_use_only_one_snapshot_set_and_snapshot_paths();
    test_unsupported_filesystem_and_bitlocker_fail_before_dependency_io();
    test_snapshot_count_mismatch_fails_before_source_open();
    test_cancellation_stops_before_first_platform_io();
    test_observed_extent_mismatch_stops_before_staging_and_dism();
    test_non_ntfs_or_bitlocker_boot_sector_stops_before_staging();
    test_static_raw_change_blocks_cleanup_success_but_discards_owned_wims();
    test_unsigned_dism_report_is_rejected_and_staging_is_discarded();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Windows NTFS shrink capture Adapter tests passed\n";
  return 0;
}

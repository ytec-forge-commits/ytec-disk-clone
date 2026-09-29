#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/windows_shrink_restore_platform.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint32_t kSector = 512U;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string utf8(const std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  check(required > 0, "test diagnostic text must be valid UTF-16");
  std::string result(static_cast<std::size_t>(required), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      required,
      nullptr,
      nullptr);
  check(written == required, "test diagnostic text must convert to UTF-8");
  return result;
}

void print_plan_error(
    const std::string_view fixture,
    const ytec::clonecore::Error& error) {
  std::cerr << fixture << " plan code="
            << static_cast<unsigned>(error.code)
            << " native=" << error.native_code
            << " operation=\"" << utf8(error.operation)
            << "\" message=\"" << utf8(error.message) << "\"\n";
}

ytec::clonecore::Error injected_error(
    const ytec::clonecore::ErrorCode code,
    const std::wstring_view operation) {
  return {
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::wstring(operation),
      .message = L"合成失敗",
  };
}

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid value{};
    value.bytes[0] = static_cast<std::byte>(++next_);
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(value);
  }

 private:
  std::uint8_t next_{};
};

class SequenceSignatureGenerator final
    : public ytec::clonecore::IMbrSignatureGenerator {
 public:
  ytec::clonecore::Result<std::uint32_t> next_signature() override {
    return ytec::clonecore::Result<std::uint32_t>::success(0x87654321U);
  }
};

void write_u32(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        (value >> (index * 8U)) & 0xffU);
  }
}

struct SourcePartition final {
  std::uint32_t table_index{};
  std::uint32_t number{};
  std::uint64_t offset{};
  std::uint64_t size{};
  std::uint8_t type{};
  bool active{};
  ytec::imageformat::TsumugiManifestPartitionRole role{
      ytec::imageformat::TsumugiManifestPartitionRole::data};
  ytec::imageformat::TsumugiManifestFileSystem file_system{
      ytec::imageformat::TsumugiManifestFileSystem::ntfs};
  std::uint64_t used{};
  std::uint64_t minimum{};
};

ytec::imageformat::TsumugiManifest manifest_for(
    const std::uint64_t source_size,
    const std::span<const SourcePartition> partitions,
    const bool windows) {
  using namespace ytec::imageformat;
  PartitionSnapshot snapshot{
      .style = PartitionTableStyle::mbr,
      .source_disk_size = source_size,
      .logical_sector_size = kSector,
  };
  PartitionTableRegion region{
      .disk_offset = 0U,
      .data = std::vector<std::byte>(kSector, std::byte{0}),
  };
  write_u32(region.data, 440U, 0x12345678U);
  for (const auto& source : partitions) {
    const std::size_t entry =
        446U + static_cast<std::size_t>(source.table_index - 1U) * 16U;
    region.data[entry] = source.active ? std::byte{0x80} : std::byte{0};
    region.data[entry + 4U] = static_cast<std::byte>(source.type);
    write_u32(
        region.data,
        entry + 8U,
        static_cast<std::uint32_t>(source.offset / kSector));
    write_u32(
        region.data,
        entry + 12U,
        static_cast<std::uint32_t>(source.size / kSector));
  }
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xaa};
  snapshot.regions.push_back(std::move(region));
  auto encoded = build_partition_snapshot_v1(snapshot);
  check(encoded.has_value(), "partition snapshot must build");

  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::shrink,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = windows
          ? TsumugiManifestFlags::source_contains_windows
          : TsumugiManifestFlags::none,
      .source_disk_size = source_size,
      .logical_sector_size = kSector,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-09T05:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = encoded.take_value(),
  };
  manifest.source_model_hash.fill(std::byte{0x31});
  manifest.source_serial_hash.fill(std::byte{0x32});
  manifest.source_state_hash.fill(std::byte{0x33});
  std::uint64_t payload_offset = 0U;
  for (const auto& source : partitions) {
    TsumugiManifestPartition partition{
        .source_table_index = source.table_index,
        .source_partition_number = source.number,
        .role = source.role,
        .file_system = source.file_system,
        .flags = TsumugiManifestPartitionFlags::selected,
        .source_offset = source.offset,
        .source_size = source.size,
        .used_bytes = source.used,
        .minimum_target_bytes = source.minimum,
        .planned_target_bytes = source.minimum,
        .payload_logical_offset = payload_offset,
        .payload_logical_length = source.file_system ==
                TsumugiManifestFileSystem::unknown
            ? source.size
            : 4096U,
        .payload_encoding = source.file_system ==
                TsumugiManifestFileSystem::unknown
            ? TsumugiManifestPayloadEncoding::exact_raw
            : TsumugiManifestPayloadEncoding::microsoft_wim_single_image,
        .payload_format_version = source.file_system ==
                TsumugiManifestFileSystem::unknown
            ? 0U
            : kTsumugiWimPayloadFormatVersion,
        .cluster_size = source.file_system ==
                TsumugiManifestFileSystem::unknown
            ? 0U
            : 4096U,
        .name_utf8 = "Synthetic",
        .label_utf8 = "Synthetic",
  };
    partition.type_id[0] = static_cast<std::byte>(source.type);
    if (source.active) {
      partition.flags = partition.flags |
          TsumugiManifestPartitionFlags::active |
          TsumugiManifestPartitionFlags::required;
    }
    if (source.role == TsumugiManifestPartitionRole::windows) {
      partition.flags = partition.flags |
          TsumugiManifestPartitionFlags::required |
          TsumugiManifestPartitionFlags::contains_windows;
    }
    payload_offset += partition.payload_logical_length;
    manifest.partitions.push_back(std::move(partition));
  }
  check(build_tsumugi_manifest_v1(manifest).has_value(),
        "manifest must be canonical");
  return manifest;
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 data_plan() {
  constexpr std::uint64_t source_size = 64ULL * kMiB;
  // Match the established shrink-layout fixture capacity. The production
  // planner reserves at least 1 GiB for data content plus metadata, so a
  // sub-GiB target is intentionally not a valid reviewed plan.
  constexpr std::uint64_t target_size = 16ULL * kGiB;
  constexpr std::array<SourcePartition, 2U> sources{{
      {
          .table_index = 1U,
          .number = 1U,
          .offset = 1ULL * kMiB,
          .size = 16ULL * kMiB,
          .type = 0x07U,
          .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
          .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
          .used = 2ULL * kMiB,
          .minimum = 8ULL * kMiB,
      },
      {
          .table_index = 2U,
          .number = 2U,
          .offset = 24ULL * kMiB,
          .size = 1ULL * kMiB,
          .type = 0x83U,
          .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
          .file_system = ytec::imageformat::TsumugiManifestFileSystem::unknown,
          .used = 1ULL * kMiB,
          .minimum = 1ULL * kMiB,
      },
  }};
  auto manifest = manifest_for(source_size, sources, false);
  SequenceGuidGenerator guids;
  SequenceSignatureGenerator signatures;
  auto plan = ytec::imageformat::
      make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
          manifest,
          target_size,
          kSector,
          ytec::imageformat::TsumugiManifestPartitionStyle::mbr,
          false,
          guids,
          signatures);
  if (!plan) {
    print_plan_error("data", plan.error());
  }
  check(plan.has_value(), "data shrink layout must build");
  return plan.take_value();
}

struct WindowsPlanFixture final {
  ytec::imageformat::TsumugiManifest manifest;
  ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 plan;
};

WindowsPlanFixture windows_gpt_plan() {
  constexpr std::uint64_t source_size = 64ULL * kMiB;
  // Windows content carries the production 10 GiB safety reserve. Reuse the
  // known-valid 16 GiB reviewed-layout capacity used by the layout tests.
  constexpr std::uint64_t target_size = 16ULL * kGiB;
  constexpr std::array<SourcePartition, 2U> sources{{
      {
          .table_index = 1U,
          .number = 1U,
          .offset = 1ULL * kMiB,
          .size = 4ULL * kMiB,
          .type = 0x07U,
          .active = true,
          .role = ytec::imageformat::TsumugiManifestPartitionRole::bios_system,
          .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
          .used = 1ULL * kMiB,
          .minimum = 2ULL * kMiB,
      },
      {
          .table_index = 2U,
          .number = 2U,
          .offset = 8ULL * kMiB,
          .size = 32ULL * kMiB,
          .type = 0x07U,
          .role = ytec::imageformat::TsumugiManifestPartitionRole::windows,
          .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
          .used = 4ULL * kMiB,
          .minimum = 16ULL * kMiB,
      },
  }};
  auto manifest = manifest_for(source_size, sources, true);
  SequenceGuidGenerator guids;
  SequenceSignatureGenerator signatures;
  auto plan = ytec::imageformat::
      make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
          manifest,
          target_size,
          kSector,
          ytec::imageformat::TsumugiManifestPartitionStyle::gpt,
          true,
          guids,
          signatures);
  if (!plan) {
    print_plan_error("Windows", plan.error());
  }
  check(plan.has_value(), "Windows MBR-to-GPT layout must build");
  return {.manifest = std::move(manifest), .plan = plan.take_value()};
}

class SparseWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  struct Record final {
    std::uint64_t offset{};
    std::vector<std::byte> bytes;
  };

  SparseWriter(const std::uint64_t size, const std::uint32_t sector)
      : size_(size), sector_(sector) {}

  std::uint64_t size_bytes() const noexcept override { return size_; }
  std::uint32_t logical_sector_size() const noexcept override {
    return sector_;
  }
  ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (fail_next_write_) {
      fail_next_write_ = false;
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed, L"合成target write"));
    }
    if (offset > size_ || bytes.size() > size_ - offset) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::invalid_argument, L"合成target range"));
    }
    records_.push_back({offset, {bytes.begin(), bytes.end()}});
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::vector<std::byte> result(length, std::byte{0});
    const std::uint64_t end = offset + length;
    for (const auto& record : records_) {
      const std::uint64_t record_end = record.offset + record.bytes.size();
      const std::uint64_t begin = (std::max)(offset, record.offset);
      const std::uint64_t overlap_end = (std::min)(end, record_end);
      if (begin >= overlap_end) {
        continue;
      }
      std::copy(
          record.bytes.begin() + static_cast<std::ptrdiff_t>(
              begin - record.offset),
          record.bytes.begin() + static_cast<std::ptrdiff_t>(
              overlap_end - record.offset),
          result.begin() + static_cast<std::ptrdiff_t>(begin - offset));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }
  ytec::clonecore::Status flush_target() override {
    return ytec::clonecore::success_status();
  }
  void fail_next_write() noexcept { fail_next_write_ = true; }

 private:
  std::uint64_t size_{};
  std::uint32_t sector_{};
  std::vector<Record> records_;
  bool fail_next_write_{};
};

ytec::clonecore::StableDiskIdentity stable_target(
    const std::uint64_t size) {
  return {
      .disk_number = 7U,
      .model = L"Synthetic target",
      .size_bytes = size,
      .logical_sector_size = kSector,
      .serial_suffix = "TGT7",
      .device_instance_id = L"SYNTHETIC\\TARGET7",
  };
}

ytec::imageformat::TsumugiRestoreDiskIdentity restore_target(
    const std::uint64_t size) {
  ytec::imageformat::TsumugiRestoreDiskIdentity identity{
      .disk_size = size,
      .logical_sector_size = kSector,
  };
  identity.stable_identity_hash[0] = std::byte{0xa5};
  return identity;
}

struct IoState final {
  std::vector<std::string> events;
  bool offline{};
  bool fail_work_paths{};
  bool fail_format{};
  bool fail_apply{};
  std::uint32_t fail_offline_attempts{};
  std::uint32_t fail_dismount_attempts{};
  bool incomplete_content_readback{};
  bool cancel_requested{};
  SparseWriter* writer{};
  std::uint64_t target_size{};
  ytec::imageformat::TsumugiRestoreDiskIdentity restore;
};

class MockIo final
    : public ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatformIo {
 public:
  explicit MockIo(IoState& state) : state_(state) {}

  ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkTargetObservation>
  observe_original_target(
      const ytec::imageformat::Sha256Digest&) override {
    state_.events.push_back("observe");
    const auto stable = stable_target(state_.target_size);
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsTsumugiShrinkTargetObservation>::success({
        .physical = {
            .target = {
                .disk_number = stable.disk_number,
                .device_path = L"\\\\.\\PhysicalDrive7",
                .device_instance_id = stable.device_instance_id,
                .model = stable.model,
                .size_bytes = stable.size_bytes,
                .sector_count = stable.size_bytes / kSector,
                .logical_sector_size = kSector,
                .physical_sector_size = 4096U,
                .bus_type = L"SATA",
                .serial_suffix = stable.serial_suffix,
                .partition_style = ytec::diskmodel::PartitionStyle::mbr,
                .offline = state_.offline,
                .read_only = false,
                .removable = false,
            },
            .target_identity = stable,
        },
        .restore_identity = state_.restore,
    });
  }
  ytec::clonecore::Status validate_work_paths_disjoint(
      const ytec::windowsapp::WindowsShrinkWorkPaths&,
      const std::uint32_t) override {
    state_.events.push_back("paths");
    return state_.fail_work_paths
        ? ytec::clonecore::Status::failure(injected_error(
              ytec::clonecore::ErrorCode::identity_mismatch,
              L"合成work path"))
        : ytec::clonecore::success_status();
  }
  ytec::clonecore::Status set_target_offline(const bool offline) override {
    state_.events.push_back(offline ? "disk-offline" : "disk-online");
    if (offline && state_.fail_offline_attempts != 0U) {
      --state_.fail_offline_attempts;
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed, L"合成disk offline"));
    }
    state_.offline = offline;
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<ytec::diskmodel::PhysicalTargetHandle>
  open_offline_target() override {
    state_.events.push_back("open-writer");
    auto writer = std::make_unique<SparseWriter>(state_.target_size, kSector);
    state_.writer = writer.get();
    const auto stable = stable_target(state_.target_size);
    return ytec::clonecore::Result<
        ytec::diskmodel::PhysicalTargetHandle>::success({
        .observed = {
            .target = {
                .disk_number = 7U,
                .size_bytes = state_.target_size,
                .logical_sector_size = kSector,
                .partition_style = ytec::diskmodel::PartitionStyle::mbr,
                .offline = true,
                .read_only = false,
                .removable = false,
            },
            .target_identity = stable,
        },
        .target = std::move(writer),
    });
  }
  ytec::clonecore::Status notify_layout_changed() override {
    state_.events.push_back("layout-notify");
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding>
  bind_online_volume(
      const std::uint32_t number,
      const std::uint64_t offset,
      const std::uint64_t size) override {
    state_.events.push_back("bind-" + std::to_string(number));
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding>::success({
        .final_target_number = number,
        .disk_number = 7U,
        .target_offset = offset,
        .target_size = size,
        .volume_device_path = L"\\\\?\\Volume{synthetic}\\",
    });
  }
  ytec::clonecore::Status format_volume(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const ytec::imageformat::TsumugiManifestFileSystem,
      const std::uint64_t) override {
    state_.events.push_back(
        "format-" + std::to_string(volume.final_target_number));
    return state_.fail_format
        ? ytec::clonecore::Status::failure(injected_error(
              ytec::clonecore::ErrorCode::io_failed, L"合成format"))
        : ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkFileSystemReadbackEvidence>
  verify_volume_readback(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const ytec::imageformat::TsumugiManifestFileSystem,
      const std::uint64_t,
      const bool content) override {
    state_.events.push_back(
        std::string(content ? "verify-content-" : "verify-fs-") +
        std::to_string(volume.final_target_number));
    return ytec::clonecore::Result<
        ytec::windowsapp::
            WindowsTsumugiShrinkFileSystemReadbackEvidence>::success({
        .directory_count = content ? 4U : 1U,
        .regular_file_count = content
            ? state_.incomplete_content_readback ? 1U : 3U
            : 0U,
        .regular_file_bytes_read = content
            ? state_.incomplete_content_readback ? 4096U : 8192U
            : 0U,
        .file_system_metadata_verified = true,
        .namespace_fully_enumerated =
            content && !state_.incomplete_content_readback,
        .every_regular_file_read_to_eof =
            content && !state_.incomplete_content_readback,
    });
  }
  ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkNtfsExtensionEvidence>
  extend_ntfs_volume_to_exact_extent_and_verify(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const std::uint64_t previous_partition_size,
      const std::uint64_t) override {
    state_.events.push_back(
        "unexpected-extend-" +
        std::to_string(volume.final_target_number));
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsTsumugiShrinkNtfsExtensionEvidence>::success({
        .previous_file_system_bytes = previous_partition_size,
        .final_file_system_bytes = volume.target_size,
        .final_partition_extent_bytes = volume.target_size,
        .exact_single_extent_reverified = true,
        .ntfs_sector_count_reverified = true,
        .flushed = true,
    });
  }
  ytec::clonecore::Status dismount_and_offline_volume(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume)
      override {
    state_.events.push_back(
        "dismount-" + std::to_string(volume.final_target_number));
    if (state_.fail_dismount_attempts != 0U) {
      --state_.fail_dismount_attempts;
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed, L"合成dismount"));
    }
    state_.offline = true;
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Status begin_owned_staged_wim(
      const std::wstring&,
      const std::uint32_t,
      const std::uint64_t) override {
    state_.events.push_back("stage-begin");
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Status append_owned_staged_wim(
      const std::uint64_t,
      const std::uint64_t,
      const bool,
      const std::span<const std::byte>) override {
    state_.events.push_back("stage-append");
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<ytec::imageformat::Sha256Digest>
  verify_and_lock_single_image_wim(const std::uint32_t) override {
    state_.events.push_back("stage-verify-lock");
    ytec::imageformat::Sha256Digest hash{};
    hash[0] = std::byte{0x55};
    return ytec::clonecore::Result<
        ytec::imageformat::Sha256Digest>::success(hash);
  }
  ytec::clonecore::Status apply_locked_wim(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding&,
      const std::wstring&,
      const ytec::imageformat::Sha256Digest&) override {
    state_.events.push_back("dism-apply");
    return state_.fail_apply
        ? ytec::clonecore::Status::failure(injected_error(
              ytec::clonecore::ErrorCode::io_failed, L"合成DISM"))
        : ytec::clonecore::success_status();
  }
  ytec::clonecore::Status discard_owned_staged_wim() override {
    state_.events.push_back("stage-discard");
    return ytec::clonecore::success_status();
  }

 private:
  IoState& state_;
};

ytec::windowsapp::WindowsTsumugiShrinkRestorePlatformRequest request_for(
    const std::uint64_t target_size,
    const IoState* state) {
  auto request = ytec::windowsapp::
      WindowsTsumugiShrinkRestorePlatformRequest{
          .expected_target = stable_target(target_size),
          .confirmation = {
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
      };
  request.expected_target_layout_hash[0] = std::byte{0x71};
  request.callbacks.cancellation_requested = [state] {
    return state != nullptr && state->cancel_requested;
  };
  return request;
}

ytec::windowsapp::WindowsShrinkWorkPaths work_paths() {
  return {
      .scratch_directory = L"E:\\YTEC\\scratch",
      .checkpoint_path = L"E:\\YTEC\\active.checkpoint",
      .log_path = L"E:\\YTEC\\operation.log",
  };
}

ytec::imageformat::TsumugiWholeDiskRestoreTarget whole_target(
    const ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& plan) {
  ytec::imageformat::TsumugiWholeDiskRestoreTarget target{
      .disk = restore_target(plan.metadata.target_size_bytes),
      .reviewed_shrink_layout = plan,
  };
  for (const auto& partition : plan.migration.target_partitions) {
    if (partition.source_table_index.has_value()) {
      target.shrink_placements.push_back({
          .source_table_index = *partition.source_table_index,
          .target_offset = partition.offset_bytes,
          .target_size = partition.size_bytes,
      });
    }
  }
  return target;
}

ytec::imageformat::TsumugiVerifiedImage verified_image(
    ytec::imageformat::TsumugiManifest manifest) {
  return {.manifest = std::move(manifest)};
}

std::unique_ptr<ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatform>
platform_for(IoState& state) {
  auto platform = ytec::windowsapp::
      make_windows_tsumugi_shrink_restore_platform_with_io(
          request_for(state.target_size, &state),
          std::make_unique<MockIo>(state));
  check(platform.has_value(), "platform factory must succeed");
  return platform.take_value();
}

void test_wim_raw_and_final_layout_are_strictly_ordered() {
  auto layout = data_plan();
  constexpr std::array<SourcePartition, 2U> sources{{
      {1U, 1U, 1ULL * kMiB, 16ULL * kMiB, 0x07U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::ntfs,
       2ULL * kMiB, 8ULL * kMiB},
      {2U, 2U, 24ULL * kMiB, 1ULL * kMiB, 0x83U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::unknown,
       1ULL * kMiB, 1ULL * kMiB},
  }};
  auto image = verified_image(manifest_for(64ULL * kMiB, sources, false));
  IoState state{
      .target_size = layout.metadata.target_size_bytes,
      .restore = restore_target(layout.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(layout);
  const auto begun = platform->begin_offline_incomplete(
      image, target, layout, work_paths());
  check(begun.has_value(), "platform begin must succeed");
  const auto wim_partition = std::find_if(
      layout.migration.target_partitions.begin(),
      layout.migration.target_partitions.end(),
      [](const auto& partition) {
        return partition.action == ytec::migrationcore::
            MigrationPartitionAction::apply_file_image;
      });
  const ytec::imageformat::TsumugiShrinkArchiveTarget archive{
      .stable_target_identity_hash = state.restore.stable_identity_hash,
      .source_table_index = *wim_partition->source_table_index,
      .source_partition_number = 1U,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .payload_format_version =
          ytec::imageformat::kTsumugiWimPayloadFormatVersion,
      .cluster_size = 4096U,
      .target_offset = wim_partition->offset_bytes,
      .target_size = wim_partition->size_bytes,
      .archive_length = 4096U,
  };
  check(platform->create_target_file_system(archive).has_value(),
        "temporary FS must build");
  check(platform->begin_staged_wim(archive, work_paths().scratch_directory)
            .has_value(),
        "WIM staging must begin");
  std::vector<std::byte> wim(4096U, std::byte{0x5a});
  check(platform->append_staged_wim(
            {.source_table_index = archive.source_table_index,
             .source_payload_offset = 0U,
             .archive_offset = 0U,
             .length = wim.size()},
            wim)
            .has_value(),
        "WIM bytes must append");
  check(platform->verify_staged_single_image_wim(archive.source_table_index)
            .has_value() &&
            platform->apply_staged_wim(archive.source_table_index)
                .has_value() &&
            platform->verify_applied_file_system_readback(archive)
                .has_value(),
        "WIM must hash-lock, apply, read back and retire");
  const auto raw_partition = std::find_if(
      layout.migration.target_partitions.begin(),
      layout.migration.target_partitions.end(),
      [](const auto& partition) {
        return partition.action == ytec::migrationcore::
            MigrationPartitionAction::copy_exact_raw;
      });
  check(platform->write_exact_raw_and_verify(
            {
                .stable_target_identity_hash =
                    state.restore.stable_identity_hash,
                .source_table_index = *raw_partition->source_table_index,
                .source_partition_number = 2U,
                .target_offset = raw_partition->offset_bytes,
                .length = raw_partition->size_bytes,
                .zero_fill = true,
            },
            {})
            .has_value(),
        "exact RAW must write and read back while offline");
  check(platform->commit_final_layout_last().has_value() && state.offline,
        "final layout must commit last and remain offline");
  const auto format = std::find(
      state.events.begin(), state.events.end(), "format-1");
  const auto apply = std::find(
      state.events.begin(), state.events.end(), "dism-apply");
  const auto readback = std::find(
      state.events.begin(), state.events.end(), "verify-content-1");
  const auto dismount = std::find(
      state.events.begin(), state.events.end(), "dismount-1");
  check(format < apply && apply < readback && readback < dismount,
        "format, DISM, content readback and retirement order must be fixed");
}

void test_generated_esp_is_prepared_during_begin_without_msr_volume() {
  auto fixture = windows_gpt_plan();
  IoState state{
      .target_size = fixture.plan.metadata.target_size_bytes,
      .restore = restore_target(fixture.plan.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(fixture.plan);
  const auto begun = platform->begin_offline_incomplete(
      verified_image(std::move(fixture.manifest)),
      target,
      fixture.plan,
      work_paths());
  check(begun.has_value(), "MBR-to-GPT platform begin must succeed");
  check(std::find(state.events.begin(), state.events.end(), "format-1") !=
            state.events.end() &&
            std::find(state.events.begin(), state.events.end(), "verify-fs-1") !=
                state.events.end() &&
            std::find(state.events.begin(), state.events.end(), "bind-2") ==
                state.events.end(),
        "generated ESP must be FAT32-prepared and MSR must never be mounted");
  platform->abort_keep_offline_incomplete();
  check(state.offline, "aborted generated-ESP begin must remain offline");
}

void test_one_file_sample_cannot_masquerade_as_complete_readback() {
  auto layout = data_plan();
  constexpr std::array<SourcePartition, 2U> sources{{
      {1U, 1U, 1ULL * kMiB, 16ULL * kMiB, 0x07U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::ntfs,
       2ULL * kMiB, 8ULL * kMiB},
      {2U, 2U, 24ULL * kMiB, 1ULL * kMiB, 0x83U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::unknown,
       1ULL * kMiB, 1ULL * kMiB},
  }};
  IoState state{
      .incomplete_content_readback = true,
      .target_size = layout.metadata.target_size_bytes,
      .restore = restore_target(layout.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(layout);
  check(platform->begin_offline_incomplete(
            verified_image(manifest_for(64ULL * kMiB, sources, false)),
            target,
            layout,
            work_paths())
            .has_value(),
        "incomplete readback fixture begin must succeed");
  const auto partition = std::find_if(
      layout.migration.target_partitions.begin(),
      layout.migration.target_partitions.end(),
      [](const auto& candidate) {
        return candidate.action == ytec::migrationcore::
            MigrationPartitionAction::apply_file_image;
      });
  const ytec::imageformat::TsumugiShrinkArchiveTarget archive{
      .stable_target_identity_hash = state.restore.stable_identity_hash,
      .source_table_index = *partition->source_table_index,
      .source_partition_number = 1U,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .payload_format_version =
          ytec::imageformat::kTsumugiWimPayloadFormatVersion,
      .cluster_size = 4096U,
      .target_offset = partition->offset_bytes,
      .target_size = partition->size_bytes,
      .archive_length = 4096U,
  };
  std::vector<std::byte> wim(4096U, std::byte{0x33});
  check(platform->create_target_file_system(archive).has_value() &&
            platform->begin_staged_wim(
                archive, work_paths().scratch_directory)
                .has_value() &&
            platform->append_staged_wim(
                {.source_table_index = archive.source_table_index,
                 .source_payload_offset = 0U,
                 .archive_offset = 0U,
                 .length = wim.size()},
                wim)
                .has_value() &&
            platform->verify_staged_single_image_wim(
                archive.source_table_index)
                .has_value() &&
            platform->apply_staged_wim(archive.source_table_index)
                .has_value(),
        "incomplete readback fixture apply must succeed");
  check(!platform->verify_applied_file_system_readback(archive).has_value(),
        "one readable file without complete namespace evidence must fail");
  const auto events_after_failure = state.events.size();
  platform->abort_keep_offline_incomplete();
  check(state.offline && state.events.size() == events_after_failure,
        "incomplete readback failure must abort offline idempotently");
}

void test_cancellation_before_exact_raw_write_aborts_offline() {
  auto layout = data_plan();
  constexpr std::array<SourcePartition, 2U> sources{{
      {1U, 1U, 1ULL * kMiB, 16ULL * kMiB, 0x07U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::ntfs,
       2ULL * kMiB, 8ULL * kMiB},
      {2U, 2U, 24ULL * kMiB, 1ULL * kMiB, 0x83U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::unknown,
       1ULL * kMiB, 1ULL * kMiB},
  }};
  IoState state{
      .target_size = layout.metadata.target_size_bytes,
      .restore = restore_target(layout.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(layout);
  check(platform->begin_offline_incomplete(
            verified_image(manifest_for(64ULL * kMiB, sources, false)),
            target,
            layout,
            work_paths())
            .has_value(),
        "cancellation fixture begin must succeed");
  const auto partition = std::find_if(
      layout.migration.target_partitions.begin(),
      layout.migration.target_partitions.end(),
      [](const auto& candidate) {
        return candidate.action == ytec::migrationcore::
            MigrationPartitionAction::copy_exact_raw;
      });
  state.cancel_requested = true;
  check(!platform->write_exact_raw_and_verify(
             {
                 .stable_target_identity_hash =
                     state.restore.stable_identity_hash,
                 .source_table_index = *partition->source_table_index,
                 .source_partition_number = 2U,
                 .target_offset = partition->offset_bytes,
                 .length = partition->size_bytes,
                 .zero_fill = true,
             },
             {})
             .has_value(),
        "safe-boundary cancellation must reject exact RAW before write");
  state.cancel_requested = false;
  const auto events_after_failure = state.events.size();
  platform->abort_keep_offline_incomplete();
  check(state.offline && state.events.size() == events_after_failure,
        "cancelled exact RAW must abort offline idempotently");
}

void test_format_failure_retires_temporary_layout_and_is_idempotent() {
  auto layout = data_plan();
  constexpr std::array<SourcePartition, 2U> sources{{
      {1U, 1U, 1ULL * kMiB, 16ULL * kMiB, 0x07U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::ntfs,
       2ULL * kMiB, 8ULL * kMiB},
      {2U, 2U, 24ULL * kMiB, 1ULL * kMiB, 0x83U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::unknown,
       1ULL * kMiB, 1ULL * kMiB},
  }};
  IoState state{
      .fail_format = true,
      .target_size = layout.metadata.target_size_bytes,
      .restore = restore_target(layout.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(layout);
  check(platform->begin_offline_incomplete(
            verified_image(manifest_for(64ULL * kMiB, sources, false)),
            target,
            layout,
            work_paths())
            .has_value(),
        "failure fixture begin must succeed");
  const auto& planned = layout.migration.target_partitions.front();
  const ytec::imageformat::TsumugiShrinkArchiveTarget archive{
      .stable_target_identity_hash = state.restore.stable_identity_hash,
      .source_table_index = *planned.source_table_index,
      .source_partition_number = 1U,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .payload_format_version =
          ytec::imageformat::kTsumugiWimPayloadFormatVersion,
      .cluster_size = 4096U,
      .target_offset = planned.offset_bytes,
      .target_size = planned.size_bytes,
      .archive_length = 4096U,
  };
  check(!platform->create_target_file_system(archive).has_value(),
        "injected FORMAT failure must fail");
  const auto events_after_failure = state.events.size();
  platform->abort_keep_offline_incomplete();
  check(state.offline && state.events.size() == events_after_failure,
        "abort must leave the target offline and be idempotent");
}

void test_abort_retries_exact_dismount_before_metadata_retirement() {
  auto layout = data_plan();
  constexpr std::array<SourcePartition, 2U> sources{{
      {1U, 1U, 1ULL * kMiB, 16ULL * kMiB, 0x07U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::ntfs,
       2ULL * kMiB, 8ULL * kMiB},
      {2U, 2U, 24ULL * kMiB, 1ULL * kMiB, 0x83U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::unknown,
       1ULL * kMiB, 1ULL * kMiB},
  }};
  IoState state{
      .fail_format = true,
      .target_size = layout.metadata.target_size_bytes,
      .restore = restore_target(layout.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(layout);
  check(platform->begin_offline_incomplete(
            verified_image(manifest_for(64ULL * kMiB, sources, false)),
            target,
            layout,
            work_paths())
            .has_value(),
        "abort-retry fixture begin must succeed");
  const auto& planned = layout.migration.target_partitions.front();
  const ytec::imageformat::TsumugiShrinkArchiveTarget archive{
      .stable_target_identity_hash = state.restore.stable_identity_hash,
      .source_table_index = *planned.source_table_index,
      .source_partition_number = 1U,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .payload_format_version =
          ytec::imageformat::kTsumugiWimPayloadFormatVersion,
      .cluster_size = 4096U,
      .target_offset = planned.offset_bytes,
      .target_size = planned.size_bytes,
      .archive_length = 4096U,
  };
  // Arm these only after begin, because begin itself proves the initial
  // offline state. The FORMAT failure then enters abort with a live binding.
  state.fail_dismount_attempts = 1U;
  state.fail_offline_attempts = 1U;
  check(!platform->create_target_file_system(archive).has_value(),
        "FORMAT failure must enter the injected abort path");
  check(!state.offline &&
            std::count(
                state.events.begin(), state.events.end(), "dismount-1") ==
                1 &&
            std::count(
                state.events.begin(), state.events.end(), "layout-notify") ==
                1,
        "failed dismount/offline must retain the live binding without "
        "retiring or notifying temporary metadata");

  platform->abort_keep_offline_incomplete();
  check(state.offline &&
            std::count(
                state.events.begin(), state.events.end(), "dismount-1") ==
                2 &&
            std::count(
                state.events.begin(), state.events.end(), "layout-notify") ==
                2,
        "idempotent retry must dismount the same binding before retiring "
        "temporary metadata");
  const auto events_after_cleanup = state.events.size();
  platform->abort_keep_offline_incomplete();
  check(state.events.size() == events_after_cleanup,
        "verified abort cleanup must become a no-op");
}

void test_unsafe_work_path_stops_before_target_touch() {
  auto layout = data_plan();
  constexpr std::array<SourcePartition, 2U> sources{{
      {1U, 1U, 1ULL * kMiB, 16ULL * kMiB, 0x07U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::ntfs,
       2ULL * kMiB, 8ULL * kMiB},
      {2U, 2U, 24ULL * kMiB, 1ULL * kMiB, 0x83U, false,
       ytec::imageformat::TsumugiManifestPartitionRole::data,
       ytec::imageformat::TsumugiManifestFileSystem::unknown,
       1ULL * kMiB, 1ULL * kMiB},
  }};
  IoState state{
      .fail_work_paths = true,
      .target_size = layout.metadata.target_size_bytes,
      .restore = restore_target(layout.metadata.target_size_bytes),
  };
  auto platform = platform_for(state);
  auto target = whole_target(layout);
  check(!platform->begin_offline_incomplete(
             verified_image(manifest_for(64ULL * kMiB, sources, false)),
             target,
             layout,
             work_paths())
             .has_value(),
        "unsafe work path must fail");
  platform->abort_keep_offline_incomplete();
  check(state.events == std::vector<std::string>({"observe", "paths"}),
        "pre-touch failure must not offline or write the target");
}

}  // namespace

int main() {
  try {
    test_wim_raw_and_final_layout_are_strictly_ordered();
    test_generated_esp_is_prepared_during_begin_without_msr_volume();
    test_one_file_sample_cannot_masquerade_as_complete_readback();
    test_cancellation_before_exact_raw_write_aborts_offline();
    test_format_failure_retires_temporary_layout_and_is_idempotent();
    test_abort_retries_exact_dismount_before_metadata_retirement();
    test_unsafe_work_path_stops_before_target_touch();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Windows shrink restore platform tests passed\n";
  return 0;
}

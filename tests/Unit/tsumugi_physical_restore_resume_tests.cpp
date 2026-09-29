#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_manifest.h"
#include "ytec/imageformat/tsumugi_physical_restore_resume.h"
#include "ytec/clonecore/gpt.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint64_t kSourceSize = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetSize = 6ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPayloadOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kChunkLength = 4096U;

struct TestFailure final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

ytec::imageformat::Sha256Digest digest(const unsigned int seed) {
  ytec::imageformat::Sha256Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>((seed + index) & 0xffU);
  }
  return value;
}

class MemoryReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemoryReader(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"synthetic source read",
          .message = L"out of range",
      });
    }
    const auto first =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  std::vector<std::byte> bytes_;
};

struct TargetState final {
  std::vector<std::byte> bytes{
      static_cast<std::size_t>(kTargetSize), std::byte{0xCC}};
  std::vector<std::uint64_t> write_offsets;
  std::size_t reads{};
  std::size_t flushes{};
  std::size_t write_calls{};
  std::optional<std::size_t> fail_write_call;
};

class MemoryTarget final : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit MemoryTarget(std::shared_ptr<TargetState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return state_->bytes.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    const auto call = state_->write_calls++;
    if (state_->fail_write_call && call == *state_->fail_write_call) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_WRITE_FAULT,
          .operation = L"synthetic target interrupted write",
          .message = L"injected before write",
      });
    }
    if (offset > state_->bytes.size() ||
        bytes.size() > state_->bytes.size() - offset) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_WRITE_FAULT,
          .operation = L"synthetic target write",
          .message = L"out of range",
      });
    }
    state_->write_offsets.push_back(offset);
    std::copy(
        bytes.begin(),
        bytes.end(),
        state_->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    ++state_->flushes;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++state_->reads;
    if (offset > state_->bytes.size() ||
        length > state_->bytes.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"synthetic target readback",
          .message = L"out of range",
      });
    }
    const auto first =
        state_->bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  std::shared_ptr<TargetState> state_;
};

ytec::clonecore::GptGuid guid(const std::byte seed) {
  ytec::clonecore::GptGuid value;
  value.bytes.fill(seed);
  return value;
}

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  explicit SequenceGuidGenerator(
      std::vector<ytec::clonecore::GptGuid> values)
      : values_(std::move(values)) {}

  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    if (next_ >= values_.size()) {
      return ytec::clonecore::Result<ytec::clonecore::GptGuid>::failure({
          .code = ytec::clonecore::ErrorCode::internal_error,
          .native_code = ERROR_NO_MORE_ITEMS,
          .operation = L"synthetic resume GPT GUID",
          .message = L"GUID exhausted",
      });
    }
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(
        values_[next_++]);
  }

 private:
  std::vector<ytec::clonecore::GptGuid> values_;
  std::size_t next_{};
};

ytec::imageformat::TsumugiManifest manifest_fixture() {
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  constexpr std::uint32_t kSourceSignature = 0x13572468U;
  write_little(disk, 440U, kSourceSignature);
  const std::size_t entry = 446U;
  disk[entry + 4U] = std::byte{0x07};
  write_little(
      disk,
      entry + 8U,
      static_cast<std::uint32_t>(kPayloadOffset / kSectorSize));
  write_little(
      disk,
      entry + 12U,
      static_cast<std::uint32_t>((2U * kChunkLength) / kSectorSize));
  disk[510U] = std::byte{0x55};
  disk[511U] = std::byte{0xAA};
  MemoryReader reader(std::move(disk));
  auto snapshot = ytec::imageformat::capture_partition_snapshot_v1(
      reader, ytec::imageformat::PartitionTableStyle::mbr);
  check(snapshot.has_value(), "MBR snapshot must build");

  ytec::imageformat::TsumugiManifest manifest{
      .mode = ytec::imageformat::TsumugiManifestMode::exact,
      .partition_style =
          ytec::imageformat::TsumugiManifestPartitionStyle::mbr,
      .flags = ytec::imageformat::TsumugiManifestFlags::none,
      .source_disk_size = kSourceSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-12T00:00:00Z",
      .app_version = "2.0.0",
      .partition_snapshot = snapshot.take_value(),
  };
  manifest.source_model_hash = digest(0x10U);
  manifest.source_serial_hash = digest(0x20U);
  manifest.source_state_hash = digest(0x30U);
  ytec::imageformat::TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
      .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
      .flags = ytec::imageformat::
          TsumugiManifestPartitionFlags::selected,
      .source_offset = kPayloadOffset,
      .source_size = 2U * kChunkLength,
      .used_bytes = 2U * kChunkLength,
      .minimum_target_bytes = 2U * kChunkLength,
      .planned_target_bytes = 2U * kChunkLength,
      .payload_logical_offset = kPayloadOffset,
      .payload_logical_length = 2U * kChunkLength,
  };
  partition.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(partition));
  check(
      ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
      "manifest must be canonical");
  return manifest;
}

ytec::imageformat::TsumugiManifest gpt_manifest_fixture() {
  using namespace ytec;
  clonecore::GptDisk source{
      .logical_sector_size = kSectorSize,
      .sector_count = kSourceSize / kSectorSize,
      .disk_guid = guid(std::byte{0x10}),
      .first_usable_lba = 34U,
      .last_usable_lba = kSourceSize / kSectorSize - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          clonecore::GptPartition{
              .entry_index = 0U,
              .type_guid = clonecore::gpt_type_basic_data(),
              .unique_guid = guid(std::byte{0x11}),
              .first_lba = kPayloadOffset / kSectorSize,
              .last_lba =
                  (kPayloadOffset + 2U * kChunkLength) / kSectorSize - 1U,
              .name = u"Data",
          },
      },
  };
  SequenceGuidGenerator ids({
      guid(std::byte{0x20}),
      guid(std::byte{0x21}),
  });
  auto generated = clonecore::make_gpt_write_plan(
      source, kSourceSize, kSectorSize, ids);
  check(generated.has_value(), "GPT snapshot plan must build");
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  for (const auto& write : generated.value().writes) {
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        disk.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  MemoryReader reader(std::move(disk));
  auto snapshot = imageformat::capture_partition_snapshot_v1(
      reader, imageformat::PartitionTableStyle::gpt);
  check(snapshot.has_value(), "GPT snapshot must build");

  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style = imageformat::TsumugiManifestPartitionStyle::gpt,
      .flags = imageformat::TsumugiManifestFlags::none,
      .source_disk_size = kSourceSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-12T00:00:00Z",
      .app_version = "2.0.0",
      .partition_snapshot = snapshot.take_value(),
  };
  manifest.source_model_hash = digest(0x10U);
  manifest.source_serial_hash = digest(0x20U);
  manifest.source_state_hash = digest(0x30U);
  const auto& source_partition =
      generated.value().target_disk.partitions.front();
  imageformat::TsumugiManifestPartition partition{
      .source_table_index = source_partition.entry_index + 1U,
      .source_partition_number = 1U,
      .role = imageformat::TsumugiManifestPartitionRole::data,
      .file_system = imageformat::TsumugiManifestFileSystem::ntfs,
      .flags = imageformat::TsumugiManifestPartitionFlags::selected,
      .source_offset = kPayloadOffset,
      .source_size = 2U * kChunkLength,
      .used_bytes = 2U * kChunkLength,
      .minimum_target_bytes = 2U * kChunkLength,
      .planned_target_bytes = 2U * kChunkLength,
      .payload_logical_offset = kPayloadOffset,
      .payload_logical_length = 2U * kChunkLength,
      .name_utf8 = "Data",
      .label_utf8 = "Data",
  };
  partition.type_id = source_partition.type_guid.bytes;
  partition.unique_id = source_partition.unique_guid.bytes;
  manifest.partitions.push_back(std::move(partition));
  check(
      imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
      "GPT manifest must be canonical");
  return manifest;
}

struct Fixture final {
  ytec::imageformat::TsumugiVerifiedImage image;
  ytec::imageformat::TsumugiRestoreDiskIdentity target;
  ytec::imageformat::TsumugiPhysicalResumeLayoutSeedV1 seed;
  std::vector<std::vector<std::byte>> plaintext;

  explicit Fixture(const bool gpt = false) {
    image.manifest = gpt ? gpt_manifest_fixture() : manifest_fixture();
    image.container.header.major_version =
        ytec::imageformat::kTsumugiMajorVersion;
    image.container.header.minor_version =
        ytec::imageformat::kTsumugiMinorVersion;
    image.container.header.payload_kind =
        ytec::imageformat::TsumugiPayloadKind::exact_disk;
    image.container.header.source_disk_size = kSourceSize;
    image.container.header.logical_sector_size = kSectorSize;
    image.container.header.physical_sector_size = 4096U;
    image.container.header.chunk_size =
        static_cast<std::uint32_t>(kChunkLength);
    image.container.header.chunk_count = 2U;
    image.container.header.header_hash = digest(0x40U);
    image.container.header.image_id[0] = std::byte{0x51};
    image.container.global_hash = digest(0x60U);
    image.container.opened_file = {
        .volume_serial = 0x11223344U,
        .file_id = {std::byte{0x71}},
        .size = 64U * 1024U,
        .last_write_time = 0x123456789U,
        .identity_from_open_handle = true,
    };
    image.container.header_hash_verified = true;
    image.container.all_chunks_verified = true;
    image.container.global_hash_verified = true;
    auto canonical =
        ytec::imageformat::build_tsumugi_manifest_v1(image.manifest);
    check(canonical.has_value(), "container manifest must build");
    image.container.manifest = canonical.take_value();
    for (std::uint64_t index = 0U; index < 2U; ++index) {
      image.container.records.push_back({
          .logical_offset = kPayloadOffset + index * kChunkLength,
          .logical_length = kChunkLength,
          .stored_offset = 4096U + index * kChunkLength,
          .stored_length = kChunkLength,
          .flags = ytec::imageformat::TsumugiChunkFlags::none,
          .compression = ytec::imageformat::ImageCompression::none,
          .nonce_counter = 0U,
          .plaintext_hash = digest(
              static_cast<unsigned int>(0x80U + index)),
      });
      plaintext.emplace_back(
          static_cast<std::size_t>(kChunkLength),
          static_cast<std::byte>(0xA0U + index));
    }
    target.stable_identity_hash = digest(0x90U);
    target.disk_size = kTargetSize;
    target.logical_sector_size = kSectorSize;
    seed.operation_id[0] = std::byte{0xB1};
    seed.plan_hash = digest(0xC0U);
  }

  void make_first_chunk_authenticated_rescue_loss() {
    image.manifest.mode =
        ytec::imageformat::TsumugiManifestMode::rescue;
    auto canonical =
        ytec::imageformat::build_tsumugi_manifest_v1(image.manifest);
    check(canonical.has_value(), "rescue manifest must be canonical");
    image.container.manifest = canonical.take_value();
    image.container.header.payload_kind =
        ytec::imageformat::TsumugiPayloadKind::rescue_disk;
    image.container.header.required_features =
        static_cast<std::uint32_t>(
            ytec::imageformat::TsumugiRequiredFeature::
                unreadable_range_map) |
        static_cast<std::uint32_t>(
            ytec::imageformat::TsumugiRequiredFeature::
                rescue_read_evidence);
    auto& missing = image.container.records[0U];
    missing.flags =
        ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled;
    missing.stored_length = 0U;
    missing.nonce_counter = 0U;
    missing.rescue_read_evidence =
        ytec::imageformat::TsumugiRescueReadEvidence{
            .forward_attempts = 1U,
            .reverse_attempts = 1U,
            .sector_attempts = 1U,
            .zero_fill_read_back_verified = true,
            .forward_native_error = ERROR_CRC,
            .reverse_native_error = ERROR_CRC,
            .sector_native_error = ERROR_CRC,
        };
    plaintext[0U].clear();
    image.unreadable_ranges = {{
        .offset = missing.logical_offset,
        .length = missing.logical_length,
    }};
    image.partial_loss = true;
  }

  ytec::imageformat::TsumugiPhysicalResumeCursorV1 cursor() const {
    auto layout =
        ytec::imageformat::make_tsumugi_physical_resume_layout_plan_v1(
            image, target, seed);
    check(layout.has_value(), "resume layout must build");
    auto segments = ytec::imageformat::
        make_tsumugi_physical_resume_payload_segments_v1(
            image.container.records, layout.value());
    check(segments.has_value(), "resume payload segments must build");
    std::uint64_t bytes{};
    for (const auto& segment : segments.value()) {
      bytes += segment.length;
    }
    return {
        .layout_seed = seed,
        .verified_payload_bytes = 0U,
        .verified_segment_count = 0U,
        .expected_payload_bytes = bytes,
        .expected_segment_count = segments.value().size(),
        .segments = segments.take_value(),
    };
  }

  ytec::imageformat::TsumugiPhysicalResumeEngineDependenciesV1 reader(
      const bool change_handle_observation = false,
      int* calls = nullptr) {
    return {
        .read_verified_image =
            [this, change_handle_observation, calls](
                const ytec::imageformat::TsumugiStreamVerifyRequest&,
                const ytec::imageformat::TsumugiVerifiedChunkCallback& chunk,
                const ytec::clonecore::DiskOperationCallbacks&,
                const ytec::imageformat::TsumugiVerifiedInspectionGate& gate) {
              if (calls != nullptr) {
                ++*calls;
              }
              auto inspection = image.container;
              if (change_handle_observation) {
                ++inspection.opened_file.last_write_time;
              }
              const auto accepted = gate(inspection);
              if (!accepted) {
                return ytec::clonecore::Result<ytec::imageformat::
                    TsumugiStreamRestoreReport>::failure(accepted.error());
              }
              std::uint64_t bytes{};
              for (std::size_t index = 0U;
                   index < inspection.records.size();
                   ++index) {
                const auto delivered = chunk(
                    inspection.records[index], plaintext[index]);
                if (!delivered) {
                  return ytec::clonecore::Result<ytec::imageformat::
                      TsumugiStreamRestoreReport>::failure(
                      delivered.error());
                }
                bytes += inspection.records[index].logical_length;
              }
              return ytec::clonecore::Result<ytec::imageformat::
                  TsumugiStreamRestoreReport>::success({
                  .inspection = std::move(inspection),
                  .delivered_logical_bytes = bytes,
                  .delivered_chunk_count = image.container.records.size(),
                  .callbacks_started_after_complete_verification = true,
              });
            },
    };
  }
};

ytec::clonecore::Result<ytec::imageformat::
    TsumugiPhysicalResumeEngineReportV1>
run_engine(
    Fixture& fixture,
    const std::shared_ptr<TargetState>& target,
    const ytec::imageformat::TsumugiPhysicalResumeCursorV1& cursor,
    const bool resume,
    const ytec::imageformat::TsumugiPhysicalResumeCheckpointCommitV1& commit,
    const bool change_handle_observation = false,
    int* reader_calls = nullptr) {
  auto durable_cursor = cursor;
  durable_cursor.durable_phase = resume
      ? ytec::imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
            prepared
      : ytec::imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
            preparing;
  return ytec::imageformat::
      execute_tsumugi_physical_whole_disk_resume_engine_v1(
          {
              .image_path = L"X:\\synthetic\\image.tsumugi",
              .storage_file_system = ytec::imageformat::
                  TsumugiImageStorageFileSystem::ntfs,
              .verification_block_bytes = 4096U,
          },
          fixture.image,
          fixture.target,
          std::make_unique<MemoryTarget>(target),
          {},
          durable_cursor,
          [&]() {
            return ytec::clonecore::Result<ytec::imageformat::
                TsumugiRestoreDiskIdentity>::success(fixture.target);
          },
          []() { return ytec::clonecore::success_status(); },
          commit,
          []() { return ytec::clonecore::success_status(); },
          fixture.reader(change_handle_observation, reader_calls));
}

ytec::imageformat::TsumugiWholeDiskRestoreLayoutPlan resume_layout(
    const Fixture& fixture) {
  auto layout =
      ytec::imageformat::make_tsumugi_physical_resume_layout_plan_v1(
          fixture.image, fixture.target, fixture.seed);
  check(layout.has_value(), "durable resume layout must build");
  return layout.take_value();
}

std::vector<ytec::imageformat::TsumugiPhysicalResumePreparationSectorV1>
capture_preparation(
    const Fixture& fixture,
    const std::shared_ptr<TargetState>& state) {
  auto layout = resume_layout(fixture);
  MemoryTarget target(state);
  auto captured =
      ytec::imageformat::capture_tsumugi_physical_resume_preparation_v1(
          layout, target);
  check(captured.has_value(), "preparation evidence must capture");
  return captured.take_value();
}

void seed_verified_payload(
    const Fixture& fixture,
    const ytec::imageformat::TsumugiPhysicalResumeCursorV1& cursor,
    const std::shared_ptr<TargetState>& state) {
  for (const auto& segment : cursor.segments) {
    const auto& record = fixture.image.container.records[
        static_cast<std::size_t>(segment.record_index)];
    const bool zero =
        (static_cast<std::uint32_t>(record.flags) &
         static_cast<std::uint32_t>(ytec::imageformat::
             TsumugiChunkFlags::unreadable_zero_filled)) != 0U;
    auto output = state->bytes.begin() +
        static_cast<std::ptrdiff_t>(segment.target_offset);
    if (zero) {
      std::fill_n(
          output, static_cast<std::size_t>(segment.length), std::byte{0});
    } else {
      const auto& plaintext = fixture.plaintext[
          static_cast<std::size_t>(segment.record_index)];
      std::copy_n(
          plaintext.begin() + static_cast<std::ptrdiff_t>(
              segment.record_plaintext_offset),
          static_cast<std::size_t>(segment.length),
          output);
    }
  }
}

void apply_publication_prefix(
    const ytec::imageformat::TsumugiWholeDiskRestoreLayoutPlan& layout,
    const std::size_t write_count,
    const std::shared_ptr<TargetState>& state) {
  std::size_t applied{};
  const auto apply = [&](const auto& writes) {
    for (const auto& write : writes) {
      if (applied++ >= write_count) {
        return false;
      }
      std::copy(
          write.bytes.begin(),
          write.bytes.end(),
          state->bytes.begin() +
              static_cast<std::ptrdiff_t>(write.offset));
    }
    return true;
  };
  if (apply(layout.staged_writes)) {
    static_cast<void>(apply(layout.commit_writes));
  }
}

ytec::clonecore::Result<ytec::imageformat::
    TsumugiPhysicalResumeEngineReportV1>
run_durable_engine(
    Fixture& fixture,
    const std::shared_ptr<TargetState>& target,
    const ytec::imageformat::TsumugiPhysicalResumeCursorV1& cursor,
    const ytec::imageformat::TsumugiPhysicalResumePhaseCommitV1&
        preparation_commit = []() {
          return ytec::clonecore::success_status();
        },
    const ytec::imageformat::TsumugiPhysicalResumeCheckpointCommitV1&
        checkpoint_commit = [](const std::uint64_t,
                               const ytec::imageformat::
                                   TsumugiPhysicalResumePayloadSegmentV1&) {
          return ytec::clonecore::success_status();
        },
    const ytec::imageformat::TsumugiPhysicalResumePhaseCommitV1&
        commit_ready_commit = []() {
          return ytec::clonecore::success_status();
        }) {
  return ytec::imageformat::
      execute_tsumugi_physical_whole_disk_resume_engine_v1(
          {
              .image_path = L"X:\\synthetic\\image.tsumugi",
              .storage_file_system = ytec::imageformat::
                  TsumugiImageStorageFileSystem::ntfs,
              .verification_block_bytes = 4096U,
          },
          fixture.image,
          fixture.target,
          std::make_unique<MemoryTarget>(target),
          {},
          cursor,
          [&]() {
            return ytec::clonecore::Result<ytec::imageformat::
                TsumugiRestoreDiskIdentity>::success(fixture.target);
          },
          preparation_commit,
          checkpoint_commit,
          commit_ready_commit,
          fixture.reader());
}

void metadata_ranges_are_removed_from_authenticated_cursor() {
  ytec::imageformat::TsumugiWholeDiskRestoreLayoutPlan layout{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .target_size_bytes = 4096U,
      .logical_sector_size = kSectorSize,
      .staged_writes = {{
          .kind = ytec::imageformat::
              TsumugiRestoreLayoutWriteKind::gpt_primary_entries,
          .offset = 1024U,
          .bytes = std::vector<std::byte>(512U),
      }},
      .commit_writes = {{
          .kind = ytec::imageformat::
              TsumugiRestoreLayoutWriteKind::mbr_sector,
          .offset = 0U,
          .bytes = std::vector<std::byte>(512U),
      }},
  };
  const std::array<ytec::imageformat::TsumugiChunkRecord, 1U> records{{
      {
          .logical_offset = 0U,
          .logical_length = 2048U,
      },
  }};
  auto segments = ytec::imageformat::
      make_tsumugi_physical_resume_payload_segments_v1(records, layout);
  check(
      segments && segments.value().size() == 2U &&
          segments.value()[0] == ytec::imageformat::
              TsumugiPhysicalResumePayloadSegmentV1{
                  .record_index = 0U,
                  .record_plaintext_offset = 512U,
                  .target_offset = 512U,
                  .length = 512U,
              } &&
          segments.value()[1] == ytec::imageformat::
              TsumugiPhysicalResumePayloadSegmentV1{
                  .record_index = 0U,
                  .record_plaintext_offset = 1536U,
                  .target_offset = 1536U,
                  .length = 512U,
              },
      "checkpoint mapping must exclude delayed primary/backup/MBR metadata and split the authenticated record");
}

void restart_verifies_prefix_without_rewrite_and_commits_last() {
  Fixture fixture;
  auto state = std::make_shared<TargetState>();
  auto cursor = fixture.cursor();
  std::uint64_t durable_count{};
  std::uint64_t durable_bytes{};
  auto interrupted = run_engine(
      fixture,
      state,
      cursor,
      false,
      [&](const std::uint64_t index,
          const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&
              segment) {
        if (index == 1U) {
          return ytec::clonecore::Status::failure({
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_WRITE_FAULT,
              .operation = L"synthetic checkpoint interruption",
              .message = L"second checkpoint failed",
          });
        }
        ++durable_count;
        durable_bytes += segment.length;
        return ytec::clonecore::success_status();
      });
  check(
      !interrupted && durable_count == 1U &&
          durable_bytes == kChunkLength,
      "interruption must retain only the segment whose readback checkpoint committed");

  cursor.verified_segment_count = durable_count;
  cursor.verified_payload_bytes = durable_bytes;
  state->write_offsets.clear();
  auto resumed = run_engine(
      fixture,
      state,
      cursor,
      true,
      [&](const std::uint64_t index,
          const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&
              segment) {
        check(index == 1U, "resume must checkpoint only the suffix");
        durable_count = index + 1U;
        durable_bytes += segment.length;
        return ytec::clonecore::success_status();
      });
  check(resumed.has_value(), "resume must complete");
  check(
      std::find(
          state->write_offsets.begin(),
          state->write_offsets.end(),
          kPayloadOffset) == state->write_offsets.end(),
      "durable prefix target bytes must be read and never rewritten");
  check(
      !state->write_offsets.empty() && state->write_offsets.back() == 0U &&
          durable_count == cursor.expected_segment_count &&
          durable_bytes == cursor.expected_payload_bytes &&
          resumed.value().final_layout_committed,
      "MBR/GPT publication must be the final write after every segment verifies");
}

void invalidated_zero_cursor_resumes_without_repeating_prepare() {
  Fixture fixture;
  auto state = std::make_shared<TargetState>();
  const auto cursor = fixture.cursor();
  auto interrupted = run_engine(
      fixture,
      state,
      cursor,
      false,
      [](const std::uint64_t index,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        check(index == 0U, "first checkpoint callback must be segment zero");
        return ytec::clonecore::Status::failure({
            .code = ytec::clonecore::ErrorCode::io_failed,
            .native_code = ERROR_WRITE_FAULT,
            .operation = L"synthetic segment-zero checkpoint interruption",
            .message = L"stop after readback before durable cursor advance",
        });
      });
  check(
      !interrupted && !state->write_offsets.empty(),
      "fixture must stop after invalidation and segment-zero readback");

  state->write_offsets.clear();
  auto resumed = run_engine(
      fixture,
      state,
      cursor,
      true,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      });
  check(
      resumed && !state->write_offsets.empty() &&
          state->write_offsets.front() == kPayloadOffset &&
          state->write_offsets.back() == 0U,
      "an incomplete zero cursor must inspect withheld metadata, restart at segment zero, and never repeat initial invalidation");
}

void preparing_wal_recovers_each_original_or_zero_sector_only() {
  Fixture fixture;
  const auto layout = resume_layout(fixture);
  auto state = std::make_shared<TargetState>();
  const auto evidence = capture_preparation(fixture, state);
  check(
      evidence.size() == ytec::imageformat::
          kTsumugiPhysicalResumeMaximumPreparationSectorsV1,
      "The two bounded invalidation ranges must capture 4096 sector digests");

  state->fail_write_call = 9U;
  MemoryTarget interrupted_target(state);
  const auto interrupted =
      ytec::imageformat::prepare_tsumugi_physical_resume_layout_v1(
          layout, evidence, interrupted_target);
  check(!interrupted, "Injected preparation interruption must fail");
  const auto mixed =
      ytec::imageformat::inspect_tsumugi_physical_resume_preparation_v1(
          layout, evidence, interrupted_target);
  check(
      mixed && mixed.value().state == ytec::imageformat::
          TsumugiPhysicalResumePreparationStateV1::original_or_zero &&
          mixed.value().zero_sector_count == 9U,
      "A preparation crash must leave only durable original-or-zero sector states");

  state->fail_write_call.reset();
  const auto recovered =
      ytec::imageformat::prepare_tsumugi_physical_resume_layout_v1(
          layout, evidence, interrupted_target);
  const auto all_zero =
      ytec::imageformat::inspect_tsumugi_physical_resume_preparation_v1(
          layout, evidence, interrupted_target);
  check(
      recovered && all_zero && all_zero.value().state == ytec::imageformat::
          TsumugiPhysicalResumePreparationStateV1::all_zero,
      "Restart must re-zero the bounded WAL ranges and verify every sector");

  auto cursor = fixture.cursor();
  cursor.durable_phase = ytec::imageformat::
      TsumugiPhysicalResumeCursorV1::DurablePhase::preparing;
  cursor.preparation_sectors = evidence;
  state->write_offsets.clear();
  const auto phase_failure = run_durable_engine(
      fixture,
      state,
      cursor,
      []() {
        return ytec::clonecore::Status::failure({
            .code = ytec::clonecore::ErrorCode::io_failed,
            .native_code = ERROR_WRITE_FAULT,
            .operation = L"synthetic prepared checkpoint",
            .message = L"durable phase commit interrupted",
        });
      });
  check(
      !phase_failure && state->write_offsets.empty(),
      "Payload must remain forbidden until the prepared phase is durable");
  const auto resumed = run_durable_engine(fixture, state, cursor);
  check(
      resumed && resumed.value().final_layout_committed,
      "The same preparing checkpoint must restart after a phase-commit crash");

  auto foreign_state = std::make_shared<TargetState>();
  const auto foreign_evidence = capture_preparation(fixture, foreign_state);
  foreign_state->bytes[static_cast<std::size_t>(
      foreign_evidence[3U].offset)] = std::byte{0x7f};
  foreign_state->write_offsets.clear();
  foreign_state->write_calls = 0U;
  MemoryTarget foreign_target(foreign_state);
  const auto foreign =
      ytec::imageformat::prepare_tsumugi_physical_resume_layout_v1(
          layout, foreign_evidence, foreign_target);
  check(
      !foreign && foreign_state->write_offsets.empty() &&
          foreign_state->write_calls == 0U,
      "A foreign/torn preparing sector must fail before every target write");
}

void gpt_commit_ready_restart_recovers_every_known_publication_window() {
  Fixture fixture(true);
  const auto layout = resume_layout(fixture);
  const std::size_t publication_writes =
      layout.staged_writes.size() + layout.commit_writes.size();
  check(
      layout.staged_writes.size() == 2U &&
          layout.commit_writes.size() == 3U,
      "GPT resume must expose the reviewed two-stage/three-commit sequence");

  for (std::size_t published = 0U;
       published <= publication_writes;
       ++published) {
    auto state = std::make_shared<TargetState>();
    auto evidence = capture_preparation(fixture, state);
    MemoryTarget target(state);
    check(
        ytec::imageformat::prepare_tsumugi_physical_resume_layout_v1(
            layout, evidence, target)
            .has_value(),
        "Commit-ready fixture must begin with verified zero metadata");
    auto cursor = fixture.cursor();
    cursor.durable_phase = ytec::imageformat::
        TsumugiPhysicalResumeCursorV1::DurablePhase::commit_ready;
    cursor.preparation_sectors = std::move(evidence);
    cursor.verified_segment_count = cursor.expected_segment_count;
    cursor.verified_payload_bytes = cursor.expected_payload_bytes;
    seed_verified_payload(fixture, cursor, state);
    apply_publication_prefix(layout, published, state);
    state->write_offsets.clear();
    state->write_calls = 0U;

    const auto resumed = run_durable_engine(fixture, state, cursor);
    MemoryTarget final_target(state);
    const auto final = ytec::imageformat::
        inspect_tsumugi_whole_disk_restore_layout_publication_v1(
            layout, final_target);
    const bool payload_rewritten = std::any_of(
        state->write_offsets.begin(),
        state->write_offsets.end(),
        [](const std::uint64_t offset) {
          return offset >= kPayloadOffset &&
              offset < kPayloadOffset + 2U * kChunkLength;
        });
    check(
        resumed && resumed.value().final_layout_committed && final &&
            final.value().state == ytec::imageformat::
                TsumugiRestoreLayoutPublicationStateV1::all_final &&
            !payload_rewritten &&
            (published != publication_writes ||
             state->write_offsets.empty()),
        "Every all-zero, known GPT prefix, and all-final restart window must verify the payload prefix and converge without replaying it");
  }
}

void commit_ready_foreign_metadata_is_read_only_rejected() {
  Fixture fixture(true);
  const auto layout = resume_layout(fixture);
  const auto make_cursor = [&](const std::shared_ptr<TargetState>& state) {
    auto evidence = capture_preparation(fixture, state);
    MemoryTarget target(state);
    check(
        ytec::imageformat::prepare_tsumugi_physical_resume_layout_v1(
            layout, evidence, target)
            .has_value(),
        "Foreign commit fixture must prepare");
    auto cursor = fixture.cursor();
    cursor.durable_phase = ytec::imageformat::
        TsumugiPhysicalResumeCursorV1::DurablePhase::commit_ready;
    cursor.preparation_sectors = std::move(evidence);
    cursor.verified_segment_count = cursor.expected_segment_count;
    cursor.verified_payload_bytes = cursor.expected_payload_bytes;
    seed_verified_payload(fixture, cursor, state);
    return cursor;
  };

  auto publication_torn = std::make_shared<TargetState>();
  auto publication_cursor = make_cursor(publication_torn);
  apply_publication_prefix(layout, 1U, publication_torn);
  publication_torn->bytes[static_cast<std::size_t>(
      layout.staged_writes.front().offset)] ^= std::byte{0x5a};
  publication_torn->write_offsets.clear();
  publication_torn->write_calls = 0U;
  const auto rejected_publication =
      run_durable_engine(fixture, publication_torn, publication_cursor);
  check(
      !rejected_publication && publication_torn->write_offsets.empty() &&
          publication_torn->write_calls == 0U,
      "Foreign/torn deterministic publication bytes must fail with zero writes");

  auto nonpublication_torn = std::make_shared<TargetState>();
  auto nonpublication_cursor = make_cursor(nonpublication_torn);
  const std::uint64_t foreign_offset = 512U * 1024U;
  nonpublication_torn->bytes[
      static_cast<std::size_t>(foreign_offset)] = std::byte{0x33};
  nonpublication_torn->write_offsets.clear();
  nonpublication_torn->write_calls = 0U;
  const auto rejected_nonpublication =
      run_durable_engine(fixture, nonpublication_torn, nonpublication_cursor);
  check(
      !rejected_nonpublication &&
          nonpublication_torn->write_offsets.empty() &&
          nonpublication_torn->write_calls == 0U,
      "Commit-ready WAL sectors outside exact publication must stay zero and reject foreign bytes before writes");
}

void prefix_tamper_and_handle_change_fail_before_suffix_write() {
  Fixture fixture;
  auto state = std::make_shared<TargetState>();
  auto cursor = fixture.cursor();
  auto interrupted = run_engine(
      fixture,
      state,
      cursor,
      false,
      [](const std::uint64_t index,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return index == 0U
            ? ytec::clonecore::success_status()
            : ytec::clonecore::Status::failure({
                  .code = ytec::clonecore::ErrorCode::io_failed,
                  .native_code = ERROR_WRITE_FAULT,
                  .operation = L"synthetic interruption",
                  .message = L"stop",
              });
      });
  check(!interrupted, "fixture must stop with one durable prefix");
  cursor.verified_segment_count = 1U;
  cursor.verified_payload_bytes = cursor.segments[0].length;
  state->bytes[static_cast<std::size_t>(kPayloadOffset)] ^=
      std::byte{0x01};
  state->write_offsets.clear();
  auto tampered = run_engine(
      fixture,
      state,
      cursor,
      true,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      });
  check(
      !tampered && state->write_offsets.empty(),
      "tampered checkpoint prefix must fail before any suffix or layout write");

  Fixture changed_handle_fixture;
  auto untouched = std::make_shared<TargetState>();
  int reader_calls{};
  auto changed_handle = run_engine(
      changed_handle_fixture,
      untouched,
      changed_handle_fixture.cursor(),
      false,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      },
      true,
      &reader_calls);
  check(
      !changed_handle && reader_calls == 1 &&
          untouched->write_offsets.empty(),
      "file identity/size/time drift between complete and restore pass must perform zero target writes");
}

void rescue_zero_filled_prefix_is_reverified_and_never_upgraded() {
  const auto interrupt_after_first = [](
                                         const std::uint64_t index,
                                         const ytec::imageformat::
                                             TsumugiPhysicalResumePayloadSegmentV1&) {
    return index == 0U
        ? ytec::clonecore::success_status()
        : ytec::clonecore::Status::failure({
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_WRITE_FAULT,
              .operation = L"synthetic rescue interruption",
              .message = L"stop after one durable rescue segment",
          });
  };

  Fixture valid;
  valid.make_first_chunk_authenticated_rescue_loss();
  auto valid_state = std::make_shared<TargetState>();
  auto valid_cursor = valid.cursor();
  auto interrupted = run_engine(
      valid,
      valid_state,
      valid_cursor,
      false,
      interrupt_after_first);
  check(!interrupted, "rescue fixture must stop after one durable prefix");
  const auto prefix_first = valid_state->bytes.begin() +
      static_cast<std::ptrdiff_t>(kPayloadOffset);
  check(
      std::all_of(
          prefix_first,
          prefix_first + static_cast<std::ptrdiff_t>(kChunkLength),
          [](const std::byte value) { return value == std::byte{0}; }),
      "authenticated unreadable rescue payload must be written as zeroes");
  valid_cursor.verified_segment_count = 1U;
  valid_cursor.verified_payload_bytes = valid_cursor.segments[0U].length;
  valid_state->write_offsets.clear();
  auto resumed = run_engine(
      valid,
      valid_state,
      valid_cursor,
      true,
      [](const std::uint64_t index,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        check(index == 1U, "rescue resume must checkpoint only the suffix");
        return ytec::clonecore::success_status();
      });
  check(
      resumed && valid.image.partial_loss &&
          valid.image.unreadable_ranges.size() == 1U &&
          std::find(
              valid_state->write_offsets.begin(),
              valid_state->write_offsets.end(),
              kPayloadOffset) == valid_state->write_offsets.end(),
      "rescue resume must re-read the zero-filled prefix without rewriting or losing partial-loss evidence");

  Fixture tampered;
  tampered.make_first_chunk_authenticated_rescue_loss();
  auto tampered_state = std::make_shared<TargetState>();
  auto tampered_cursor = tampered.cursor();
  check(
      !run_engine(
          tampered,
          tampered_state,
          tampered_cursor,
          false,
          interrupt_after_first),
      "tamper fixture must stop after one durable rescue prefix");
  tampered_cursor.verified_segment_count = 1U;
  tampered_cursor.verified_payload_bytes =
      tampered_cursor.segments[0U].length;
  tampered_state->bytes[static_cast<std::size_t>(kPayloadOffset)] =
      std::byte{0x7F};
  tampered_state->write_offsets.clear();
  auto rejected = run_engine(
      tampered,
      tampered_state,
      tampered_cursor,
      true,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      });
  check(
      !rejected && tampered_state->write_offsets.empty(),
      "tampered rescue zero-filled prefix must fail before suffix or layout write");
}

void mode_and_partial_loss_evidence_are_rejected_before_io() {
  Fixture mismatched_mode;
  mismatched_mode.image.manifest.mode =
      ytec::imageformat::TsumugiManifestMode::rescue;
  auto mismatched_state = std::make_shared<TargetState>();
  int mismatched_reader_calls{};
  auto mode_rejected = run_engine(
      mismatched_mode,
      mismatched_state,
      mismatched_mode.cursor(),
      false,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      },
      false,
      &mismatched_reader_calls);
  check(
      !mode_rejected && mismatched_reader_calls == 0 &&
          mismatched_state->write_offsets.empty() &&
          mismatched_state->reads == 0U && mismatched_state->flushes == 0U,
      "manifest/payload mode mismatch must fail before reader or target I/O");

  Fixture exact_with_loss;
  exact_with_loss.image.partial_loss = true;
  exact_with_loss.image.unreadable_ranges = {{
      .offset = exact_with_loss.image.container.records[0U].logical_offset,
      .length = exact_with_loss.image.container.records[0U].logical_length,
  }};
  auto exact_state = std::make_shared<TargetState>();
  int exact_reader_calls{};
  auto classification_rejected = run_engine(
      exact_with_loss,
      exact_state,
      exact_with_loss.cursor(),
      false,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      },
      false,
      &exact_reader_calls);
  check(
      !classification_rejected && exact_reader_calls == 0 &&
          exact_state->write_offsets.empty() && exact_state->reads == 0U &&
          exact_state->flushes == 0U,
      "exact mode must not be upgraded to carry unauthenticated rescue loss evidence");

  Fixture unsafe_target;
  unsafe_target.target.is_usb_memory = true;
  auto unsafe_state = std::make_shared<TargetState>();
  int unsafe_reader_calls{};
  auto unsafe_rejected = run_engine(
      unsafe_target,
      unsafe_state,
      unsafe_target.cursor(),
      false,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      },
      false,
      &unsafe_reader_calls);
  check(
      !unsafe_rejected && unsafe_reader_calls == 0 &&
          unsafe_state->write_offsets.empty() && unsafe_state->reads == 0U &&
          unsafe_state->flushes == 0U,
      "The public low-level engine must reject unsupported target flags before reader or target I/O");
}

void nonce_cursor_and_4kn_are_fail_closed() {
  Fixture nonce;
  nonce.image.container.header.required_features =
      static_cast<std::uint32_t>(
          ytec::imageformat::TsumugiRequiredFeature::encrypted);
  nonce.image.container.header.base_nonce[0] = std::byte{1};
  nonce.image.container.metadata_authenticated = true;
  nonce.image.container.records[0].nonce_counter = 1U;
  nonce.image.container.records[1].nonce_counter = 1U;
  auto state = std::make_shared<TargetState>();
  int reader_calls{};
  auto replay = run_engine(
      nonce,
      state,
      nonce.cursor(),
      false,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      },
      false,
      &reader_calls);
  check(
      !replay && reader_calls == 0 && state->write_offsets.empty(),
      "duplicate encrypted nonce counter must fail before reader or target I/O");

  Fixture bad_cursor;
  auto overflow = bad_cursor.cursor();
  overflow.verified_segment_count = overflow.expected_segment_count + 1U;
  auto cursor_rejected = run_engine(
      bad_cursor,
      std::make_shared<TargetState>(),
      overflow,
      true,
      [](const std::uint64_t,
         const ytec::imageformat::TsumugiPhysicalResumePayloadSegmentV1&) {
        return ytec::clonecore::success_status();
      });
  check(!cursor_rejected, "out-of-range durable cursor must fail closed");

  auto four_kn_target = bad_cursor.target;
  four_kn_target.logical_sector_size = 4096U;
  auto four_kn =
      ytec::imageformat::make_tsumugi_physical_resume_layout_plan_v1(
          bad_cursor.image,
          four_kn_target,
          bad_cursor.seed);
  check(!four_kn, "4Kn completion must not be claimed by this backend");
}

void full_verification_failure_performs_zero_target_io() {
  Fixture fixture;
  auto state = std::make_shared<TargetState>();
  int reader_calls{};
  int target_reidentifications{};
  auto failed = ytec::imageformat::
      execute_tsumugi_physical_whole_disk_resume_engine_v1(
          {
              .image_path = L"X:\\synthetic\\image.tsumugi",
              .storage_file_system = ytec::imageformat::
                  TsumugiImageStorageFileSystem::ntfs,
              .verification_block_bytes = 4096U,
          },
          fixture.image,
          fixture.target,
          std::make_unique<MemoryTarget>(state),
          {},
          fixture.cursor(),
          [&]() {
            ++target_reidentifications;
            return ytec::clonecore::Result<ytec::imageformat::
                TsumugiRestoreDiskIdentity>::success(fixture.target);
          },
          []() { return ytec::clonecore::success_status(); },
          [](const std::uint64_t,
             const ytec::imageformat::
                 TsumugiPhysicalResumePayloadSegmentV1&) {
            return ytec::clonecore::success_status();
          },
          []() { return ytec::clonecore::success_status(); },
          {
              .read_verified_image =
                  [&](const ytec::imageformat::
                          TsumugiStreamVerifyRequest&,
                      const ytec::imageformat::
                          TsumugiVerifiedChunkCallback&,
                      const ytec::clonecore::DiskOperationCallbacks&,
                      const ytec::imageformat::
                          TsumugiVerifiedInspectionGate&) {
                    ++reader_calls;
                    return ytec::clonecore::Result<ytec::imageformat::
                        TsumugiStreamRestoreReport>::failure({
                        .code = ytec::clonecore::ErrorCode::
                            verification_failed,
                        .native_code = ERROR_CRC,
                        .operation = L"synthetic complete verification",
                        .message = L"failed before gate",
                    });
                  },
          });
  check(
      !failed && reader_calls == 1 && state->write_offsets.empty() &&
          target_reidentifications == 0 && state->reads == 0U &&
          state->flushes == 0U,
      "complete image verification failure must leave target I/O at zero");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"metadata_ranges_are_removed_from_authenticated_cursor",
       metadata_ranges_are_removed_from_authenticated_cursor},
      {"restart_verifies_prefix_without_rewrite_and_commits_last",
       restart_verifies_prefix_without_rewrite_and_commits_last},
      {"invalidated_zero_cursor_resumes_without_repeating_prepare",
       invalidated_zero_cursor_resumes_without_repeating_prepare},
      {"preparing_wal_recovers_each_original_or_zero_sector_only",
       preparing_wal_recovers_each_original_or_zero_sector_only},
      {"gpt_commit_ready_restart_recovers_every_known_publication_window",
       gpt_commit_ready_restart_recovers_every_known_publication_window},
      {"commit_ready_foreign_metadata_is_read_only_rejected",
       commit_ready_foreign_metadata_is_read_only_rejected},
      {"prefix_tamper_and_handle_change_fail_before_suffix_write",
       prefix_tamper_and_handle_change_fail_before_suffix_write},
      {"rescue_zero_filled_prefix_is_reverified_and_never_upgraded",
       rescue_zero_filled_prefix_is_reverified_and_never_upgraded},
      {"mode_and_partial_loss_evidence_are_rejected_before_io",
       mode_and_partial_loss_evidence_are_rejected_before_io},
      {"nonce_cursor_and_4kn_are_fail_closed",
       nonce_cursor_and_4kn_are_fail_closed},
      {"full_verification_failure_performs_zero_target_io",
       full_verification_failure_performs_zero_target_io},
  };
  int failures{};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

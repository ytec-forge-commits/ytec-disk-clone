#include "ytec/winpeapp/direct_image_create.h"
#include "ytec/winpeapp/direct_image_create_resume.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/imageformat/tsumugi_image_service.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint64_t kDiskSize = 16ULL * 1024ULL * 1024ULL;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::vector<wchar_t> buffer(32768U, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    check(length != 0U && length < buffer.size(), "temp path is required");
    path_ = std::filesystem::path(buffer.data()) /
        (L"ytec-winpe-tsumugi-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    check(std::filesystem::create_directory(path_, error) && !error,
          "temporary directory must be created");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::wstring image(const std::wstring& name) const {
    return (path_ / name).wstring();
  }

 private:
  std::filesystem::path path_;
};

class SharedDiskReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  SharedDiskReader(
      std::shared_ptr<std::vector<std::byte>> storage,
      const bool mutate_mbr_after_payload,
      std::shared_ptr<std::vector<std::pair<std::uint64_t, std::size_t>>>
          reads)
      : storage_(std::move(storage)),
        mutate_mbr_after_payload_(mutate_mbr_after_payload),
        reads_(std::move(reads)) {}

  std::uint64_t size_bytes() const noexcept override {
    return storage_->size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > storage_->size() || length > storage_->size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成PE Source読取り",
          .message = L"範囲外です",
      });
    }
    reads_->emplace_back(offset, length);
    const auto first = storage_->begin() + static_cast<std::ptrdiff_t>(offset);
    std::vector<std::byte> result(
        first, first + static_cast<std::ptrdiff_t>(length));
    if (mutate_mbr_after_payload_ && !mutated_ && offset >= 1024U * 1024U) {
      (*storage_)[440] ^= std::byte{0x01};
      mutated_ = true;
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

 private:
  std::shared_ptr<std::vector<std::byte>> storage_;
  bool mutate_mbr_after_payload_{};
  std::shared_ptr<std::vector<std::pair<std::uint64_t, std::size_t>>> reads_;
  mutable bool mutated_{};
};

struct RescueStagingState final {
  std::size_t factory_calls{};
  std::size_t seal_calls{};
  std::size_t discard_calls{};
  std::size_t destination_validation_calls{};
  ytec::imageformat::WindowsTsumugiRescueStagingRequest request;
};

class MemoryRescueStaging final
    : public ytec::imageformat::ITsumugiRescueStagingSession {
 public:
  MemoryRescueStaging(
      ytec::imageformat::WindowsTsumugiRescueStagingRequest request,
      std::shared_ptr<RescueStagingState> state)
      : request_(std::move(request)),
        state_(std::move(state)),
        bytes_(static_cast<std::size_t>(request_.source_disk_size),
               std::byte{0}) {}

  std::uint64_t size_bytes() const noexcept override {
    return request_.source_disk_size;
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return request_.logical_sector_size;
  }

  ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    return request_.source_model_hash;
  }

  ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    return request_.source_serial_hash;
  }

  ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    return request_.source_state_hash;
  }

  ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (sealed_ || discarded_ || offset > bytes_.size() ||
        bytes.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_WRITE_FAULT,
          .operation = L"合成PE救出一時領域write",
          .message = L"状態または範囲が不正です",
      });
    }
    std::copy(
        bytes.begin(), bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    flushed_ = false;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    return read_range(offset, length, false);
  }

  ytec::clonecore::Status flush_target() override {
    if (sealed_ || discarded_) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_INVALID_STATE,
          .operation = L"合成PE救出一時領域flush",
          .message = L"状態が不正です",
      });
    }
    flushed_ = true;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status seal_for_image_read() override {
    ++state_->seal_calls;
    if (!flushed_ || discarded_ || sealed_) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::verification_failed,
          .native_code = ERROR_INVALID_STATE,
          .operation = L"合成PE救出一時領域seal",
          .message = L"状態が不正です",
      });
    }
    sealed_ = true;
    return ytec::clonecore::success_status();
  }

  bool sealed_for_image_read() const noexcept override { return sealed_; }

  ytec::clonecore::Status discard_owned_staging() noexcept override {
    ++state_->discard_calls;
    if (!discarded_) {
      bytes_.clear();
      discarded_ = true;
      sealed_ = false;
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status validate_image_destination_before_commit(
      const std::uint64_t expected_owned_partial_bytes) override {
    ++state_->destination_validation_calls;
    if (!discarded_ || expected_owned_partial_bytes == 0U) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::identity_mismatch,
          .native_code = ERROR_FILE_INVALID,
          .operation = L"合成PE救出保存先再識別",
          .message = L"一時領域未破棄またはpartial長不正です",
      });
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    return read_range(offset, length, true);
  }

 private:
  ytec::clonecore::Result<std::vector<std::byte>> read_range(
      const std::uint64_t offset,
      const std::size_t length,
      const bool require_sealed) const {
    if (discarded_ || (require_sealed && !sealed_) ||
        (!require_sealed && sealed_) || offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成PE救出一時領域read",
          .message = L"状態または範囲が不正です",
      });
    }
    const auto begin =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            begin, begin + static_cast<std::ptrdiff_t>(length)));
  }

  ytec::imageformat::WindowsTsumugiRescueStagingRequest request_;
  std::shared_ptr<RescueStagingState> state_;
  std::vector<std::byte> bytes_;
  bool flushed_{};
  bool sealed_{};
  bool discarded_{};
};

class SequentialGuidGenerator final
    : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid result;
    result.bytes[0] = static_cast<std::byte>(next_++);
    result.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(
        result);
  }

 private:
  std::uint8_t next_{1U};
};

ytec::clonecore::GptGuid guid(const std::uint8_t value) {
  ytec::clonecore::GptGuid result;
  result.bytes[0] = static_cast<std::byte>(value);
  result.bytes[15] = std::byte{0x5A};
  return result;
}

std::shared_ptr<std::vector<std::byte>> mbr_storage() {
  auto storage = std::make_shared<std::vector<std::byte>>(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  write_little(*storage, 440U, 0x1234ABCDU);
  (*storage)[446U] = std::byte{0x80};
  (*storage)[450U] = std::byte{0x07};
  write_little(*storage, 454U, 2048U);
  write_little(*storage, 458U, 2048U);
  (*storage)[510U] = std::byte{0x55};
  (*storage)[511U] = std::byte{0xAA};
  for (std::size_t index = 1024U * 1024U;
       index < 2U * 1024U * 1024U; ++index) {
    (*storage)[index] = static_cast<std::byte>(index % 251U);
  }
  return storage;
}

std::shared_ptr<std::vector<std::byte>> gpt_storage() {
  ytec::clonecore::GptDisk layout{
      .logical_sector_size = kSectorSize,
      .sector_count = kDiskSize / kSectorSize,
      .disk_guid = guid(0x10U),
      .first_usable_lba = 34U,
      .last_usable_lba = kDiskSize / kSectorSize - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          ytec::clonecore::GptPartition{
              .entry_index = 0U,
              .type_guid = ytec::clonecore::gpt_type_efi_system(),
              .unique_guid = guid(0x20U),
              .first_lba = 2048U,
              .last_lba = 3071U,
              .name = u"ESP",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 1U,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = guid(0x30U),
              .first_lba = 4096U,
              .last_lba = 6143U,
              .name = u"Data",
          },
      },
  };
  SequentialGuidGenerator generator;
  const auto plan = ytec::clonecore::make_gpt_write_plan(
      layout, kDiskSize, kSectorSize, generator);
  check(plan.has_value(), "synthetic GPT plan must build");
  auto storage = std::make_shared<std::vector<std::byte>>(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  for (const auto& write : plan.value().writes) {
    check(write.offset <= storage->size() &&
              write.bytes.size() <= storage->size() - write.offset,
          "GPT metadata must fit");
    std::copy(
        write.bytes.begin(), write.bytes.end(),
        storage->begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  return storage;
}

ytec::diskmodel::DiskInfo source_disk(
    const ytec::diskmodel::PartitionStyle style) {
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 7U,
      .device_path = L"\\\\.\\PhysicalDrive7",
      .device_instance_id = L"SYNTHETIC\\PE\\SOURCE7",
      .model = L"Y-TEC Synthetic PE Source",
      .size_bytes = kDiskSize,
      .sector_count = kDiskSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "PE000007",
      .partition_style = style,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  if (style == ytec::diskmodel::PartitionStyle::mbr) {
    disk.partitions.push_back({
        .number = 1U,
        .offset_bytes = 1024U * 1024U,
        .size_bytes = 1024U * 1024U,
        .style = style,
        .type = L"0x07",
        .bootable = true,
    });
  } else {
    disk.partitions.push_back({
        .number = 1U,
        .offset_bytes = 1024U * 1024U,
        .size_bytes = 512U * 1024U,
        .style = style,
        .type = L"ESP",
        .name = L"ESP",
    });
    disk.partitions.push_back({
        .number = 2U,
        .offset_bytes = 2U * 1024U * 1024U,
        .size_bytes = 1024U * 1024U,
        .style = style,
        .type = L"Basic Data",
        .name = L"Data",
    });
  }
  return disk;
}

struct DependencyState final {
  std::size_t read_only_calls{};
  std::shared_ptr<std::vector<std::pair<std::uint64_t, std::size_t>>> reads{
      std::make_shared<
          std::vector<std::pair<std::uint64_t, std::size_t>>>()};
  std::vector<ytec::imageformat::WindowsTsumugiDestinationGuardRequest>
      guards;
  std::shared_ptr<RescueStagingState> rescue_staging{
      std::make_shared<RescueStagingState>()};
};

ytec::winpeapp::DirectImageCreateDependencies dependencies_for(
    const ytec::diskmodel::DiskInfo& reviewed,
    std::shared_ptr<std::vector<std::byte>> storage,
    const std::shared_ptr<DependencyState>& state,
    const bool mutate_mbr_after_payload = false) {
  return {
      .set_source_read_only =
          [state](const ytec::clonecore::StableDiskIdentity& expected,
                  const bool read_only) {
            ++state->read_only_calls;
            if (expected.disk_number != 7U || !read_only) {
              return ytec::clonecore::Status::failure({
                  .code = ytec::clonecore::ErrorCode::identity_mismatch,
                  .native_code = ERROR_INVALID_DATA,
                  .operation = L"合成read-only化",
                  .message = L"対象不一致",
              });
            }
            return ytec::clonecore::success_status();
          },
      .open_read_only_source =
          [reviewed,
           storage = std::move(storage),
           mutate_mbr_after_payload,
           state](
              const ytec::clonecore::StableDiskIdentity& expected) {
            auto observed = reviewed;
            observed.read_only = true;
            auto identity = ytec::diskmodel::make_stable_disk_identity(
                observed, observed.is_system_disk);
            if (!identity || expected.disk_number != observed.disk_number) {
              return ytec::clonecore::Result<
                  ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::failure({
                  .code = ytec::clonecore::ErrorCode::identity_mismatch,
                  .native_code = ERROR_INVALID_DATA,
                  .operation = L"合成Source再識別",
                  .message = L"対象不一致",
              });
            }
            return ytec::clonecore::Result<
                ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success({
                .observed = {
                    .observed = std::move(observed),
                    .identity = identity.take_value(),
                },
                .reader = std::make_unique<SharedDiskReader>(
                    storage, mutate_mbr_after_payload, state->reads),
            });
          },
      .query_destination_file_system =
          [](const std::wstring&) {
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiImageStorageFileSystem>::success(
                ytec::imageformat::TsumugiImageStorageFileSystem::ntfs);
          },
      .validate_destination =
          [state](const auto& guard) {
            state->guards.push_back(guard);
            return ytec::clonecore::success_status();
          },
      .make_rescue_staging =
          [state](const auto& request) {
            ++state->rescue_staging->factory_calls;
            state->rescue_staging->request = request;
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::imageformat::ITsumugiRescueStagingSession>>::success(
                std::make_unique<MemoryRescueStaging>(
                    request, state->rescue_staging));
          },
  };
}

ytec::winpeapp::DirectImageCreateRequest request_for(
    ytec::diskmodel::DiskInfo source,
    std::wstring path,
    std::vector<ytec::clonecore::DiskOperationStage>* progress = nullptr) {
  ytec::winpeapp::DirectImageCreateRequest request{
      .selected_source = std::move(source),
      .final_path = std::move(path),
      .created_utc = "2026-08-08T12:00:00Z",
      .app_version = "1.0.0-test",
  };
  if (progress != nullptr) {
    request.callbacks.progress = [progress](const auto& value) {
      progress->push_back(value.stage);
    };
  }
  return request;
}

ytec::winpeapp::DirectImageCreateResumeContinuityV1 continuity_fixture(
    const bool encrypted) {
  ytec::winpeapp::DirectImageCreateResumeContinuityV1 continuity{
      .created_utc = "2026-08-24T10:20:30Z",
      .app_version = "1.0.0-test",
      .verification_mode =
          ytec::imageformat::TsumugiCreateVerificationMode::fast,
      .encrypted = encrypted,
  };
  continuity.image_id[0] = std::byte{0xAB};
  continuity.image_id[15] = std::byte{0x5C};
  if (encrypted) {
    continuity.argon2.salt[0] = std::byte{0x91};
    continuity.argon2.salt[15] = std::byte{0x2E};
    continuity.base_nonce[0] = std::byte{0x77};
    continuity.base_nonce[11] = std::byte{0x08};
  }
  return continuity;
}

void resume_continuity_roundtrips_without_secret_material() {
  for (const bool encrypted : {false, true}) {
    auto input = continuity_fixture(encrypted);
    if (encrypted) {
      input.selected_partition_numbers = {2U, 4U, 128U};
    }
    const auto token = ytec::winpeapp::
        build_direct_image_create_resume_continuity_v1(input);
    check(token.has_value(), "canonical continuity token must build");
    check(token.value().find(L"password") == std::wstring::npos &&
              token.value().find(L"key=") == std::wstring::npos,
          "continuity token must not persist password or key fields");
    const auto parsed = ytec::winpeapp::
        parse_direct_image_create_resume_continuity_v1(token.value());
    check(parsed.has_value() &&
              parsed.value().created_utc == input.created_utc &&
              parsed.value().app_version == input.app_version &&
              parsed.value().verification_mode == input.verification_mode &&
              parsed.value().encrypted == encrypted &&
              parsed.value().selected_partition_numbers ==
                  input.selected_partition_numbers &&
              parsed.value().image_id == input.image_id &&
              parsed.value().argon2.memory_kib == input.argon2.memory_kib &&
              parsed.value().argon2.iterations == input.argon2.iterations &&
              parsed.value().argon2.parallelism == input.argon2.parallelism &&
              parsed.value().argon2.salt == input.argon2.salt &&
              parsed.value().base_nonce == input.base_nonce,
          "canonical continuity fields must roundtrip exactly");
    check(
        (encrypted && token.value().find(L"|Q") != std::wstring::npos) ||
            (!encrypted && token.value().find(L"|Q") == std::wstring::npos),
        "partial selection must use the private Q bitmap while legacy whole-disk tokens remain unchanged");
  }
}

void resume_continuity_rejects_noncanonical_unknown_and_oversized_input() {
  const auto input = continuity_fixture(true);
  const auto built = ytec::winpeapp::
      build_direct_image_create_resume_continuity_v1(input);
  check(built.has_value(), "encrypted continuity fixture must build");

  auto unknown = built.value();
  const auto version = unknown.find(L"V1");
  check(version != std::wstring::npos, "continuity version marker must exist");
  unknown.replace(version, 2U, L"V2");
  check(!ytec::winpeapp::parse_direct_image_create_resume_continuity_v1(
             unknown),
        "unknown continuity version must fail closed");

  auto uppercase = built.value();
  const auto lowercase_hex = uppercase.find(L"ab");
  check(lowercase_hex != std::wstring::npos,
        "continuity fixture must contain lowercase hex");
  uppercase.replace(lowercase_hex, 2U, L"AB");
  check(!ytec::winpeapp::parse_direct_image_create_resume_continuity_v1(
             uppercase),
        "noncanonical uppercase hex must fail closed");

  auto extra_secret_field = built.value() + L"|password=not-persisted";
  check(!ytec::winpeapp::parse_direct_image_create_resume_continuity_v1(
             extra_secret_field),
        "unrecognized or secret-shaped fields must fail closed");
  check(!ytec::winpeapp::parse_direct_image_create_resume_continuity_v1(
             std::wstring(513U, L'A')),
        "oversized continuity input must fail before unbounded parsing");

  auto zero_image_id = continuity_fixture(false);
  zero_image_id.image_id.fill(std::byte{0});
  check(!ytec::winpeapp::build_direct_image_create_resume_continuity_v1(
             zero_image_id),
        "zero image identity must not be persisted");
  auto missing_encryption_material = continuity_fixture(true);
  missing_encryption_material.argon2.salt.fill(std::byte{0});
  check(!ytec::winpeapp::build_direct_image_create_resume_continuity_v1(
             missing_encryption_material),
        "encrypted continuity must bind public salt and nonce material");

  auto selected = continuity_fixture(false);
  selected.selected_partition_numbers = {2U, 4U, 128U};
  const auto selected_token = ytec::winpeapp::
      build_direct_image_create_resume_continuity_v1(selected);
  check(selected_token.has_value(), "partial continuity fixture must build");
  const auto q = selected_token.value().find(L"|Q");
  check(q != std::wstring::npos, "partial continuity must contain Q bitmap");
  auto zero_bitmap = selected_token.value();
  zero_bitmap.replace(q + 2U, 32U, 32U, L'0');
  check(!ytec::winpeapp::parse_direct_image_create_resume_continuity_v1(
             zero_bitmap),
        "empty partition bitmap must fail closed");
  auto uppercase_bitmap = selected_token.value();
  const auto first_bitmap_hex = uppercase_bitmap.find_first_of(
      L"abcdef", q + 2U);
  check(first_bitmap_hex != std::wstring::npos,
        "partial bitmap fixture must contain lowercase hex");
  uppercase_bitmap[first_bitmap_hex] = static_cast<wchar_t>(
      uppercase_bitmap[first_bitmap_hex] - L'a' + L'A');
  check(!ytec::winpeapp::parse_direct_image_create_resume_continuity_v1(
             uppercase_bitmap),
        "noncanonical uppercase partition bitmap must fail closed");
}

void mbr_exact_image_is_fully_verified_and_committed() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"mbr-exact.tsumugi");
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  std::vector<ytec::clonecore::DiskOperationStage> progress;
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request_for(source, path, &progress),
      dependencies_for(source, mbr_storage(), state));
  check(result.has_value(), "MBR exact image must succeed");
  check(result.value().source_read_only_verified &&
            result.value().source_left_read_only &&
            result.value().layout_revalidated_before_commit,
        "source protection and final layout recheck must be reported");
  check(result.value().imaged_partition_count == 1U &&
            result.value().logical_payload_bytes == 1024U * 1024U,
        "the complete MBR partition must be planned");
  check(result.value().image.selected_verification_passed &&
            result.value().image.complete_verification_passed &&
             result.value().image.stream.committed &&
             result.value().image.stream.all_chunks_read_back_verified &&
             result.value().image.stream.all_chunks_authenticated_and_hashed &&
             result.value().image.stream.global_hash_read_back_verified &&
            result.value().image.stream.final_metadata_read_back_verified &&
            result.value().image.stream.final_complete_scan_performed,
        "the final image must be completely read-back verified");
  check(state->read_only_calls == 1U && state->guards.size() == 3U &&
            state->guards[0].phase == ytec::imageformat::
                WindowsTsumugiDestinationGuardPhase::before_stage &&
            state->guards[1].required_available_bytes >
                result.value().logical_payload_bytes &&
            state->guards[2].phase == ytec::imageformat::
                WindowsTsumugiDestinationGuardPhase::
                    before_commit_owned_partial &&
            state->guards[2].expected_owned_partial_bytes > 0U,
        "destination must be checked before planning, before stage, and before commit");
  check(!progress.empty() &&
            progress.back() == ytec::clonecore::DiskOperationStage::completed,
        "progress callbacks must reach completion");

  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
  });
  check(verified.has_value() &&
            verified.value().manifest.mode ==
                ytec::imageformat::TsumugiManifestMode::exact &&
            verified.value().manifest.partition_style ==
                ytec::imageformat::TsumugiManifestPartitionStyle::mbr &&
            verified.value().manifest.partitions.size() == 1U,
        "the committed MBR image must reopen as canonical Tsumugi v1");
}

void mbr_fast_image_keeps_required_gates_without_complete_claim() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"mbr-fast.tsumugi");
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  auto request = request_for(source, path);
  request.verification_mode =
      ytec::imageformat::TsumugiCreateVerificationMode::fast;
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request, dependencies_for(source, mbr_storage(), state));
  check(result.has_value(), "PE fast image creation must succeed");
  check(
      ytec::imageformat::selected_tsumugi_creation_verification_passed(
          result.value().image) &&
          !result.value().image.complete_verification_passed &&
           result.value().image.stream.verification_mode ==
               ytec::imageformat::TsumugiCreateVerificationMode::fast &&
           result.value().image.stream.all_chunks_authenticated_and_hashed &&
           result.value().image.stream.final_metadata_read_back_verified &&
          !result.value().image.stream.final_complete_scan_performed &&
          result.value().image.stream.committed,
      "PE fast mode must preserve every required gate without claiming the omitted scan");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
  });
  check(verified.has_value(),
        "PE fast-created image must pass an independent complete verification");
}

void gpt_exact_image_preserves_partition_roles() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"gpt-exact.tsumugi");
  auto source = source_disk(ytec::diskmodel::PartitionStyle::gpt);
  auto state = std::make_shared<DependencyState>();
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request_for(source, path),
      dependencies_for(source, gpt_storage(), state));
  check(result.has_value(), "GPT exact image must succeed");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
  });
  check(verified.has_value() &&
            verified.value().manifest.partition_style ==
                ytec::imageformat::TsumugiManifestPartitionStyle::gpt &&
            verified.value().manifest.partitions.size() == 2U &&
            verified.value().manifest.partitions[0].role ==
                ytec::imageformat::TsumugiManifestPartitionRole::efi_system &&
            verified.value().manifest.partitions[1].role ==
                ytec::imageformat::TsumugiManifestPartitionRole::data,
        "GPT type GUID roles must be retained in the typed manifest");
}

void gpt_partial_image_reads_only_selected_partition_payload() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"gpt-partition-2.tsumugi");
  auto source = source_disk(ytec::diskmodel::PartitionStyle::gpt);
  auto state = std::make_shared<DependencyState>();
  auto request = request_for(source, path);
  request.selected_partition_numbers = {2U};
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request, dependencies_for(source, gpt_storage(), state));
  check(result.has_value(), "selected GPT partition image must succeed");
  check(
      result.value().imaged_partition_count == 1U &&
          result.value().logical_payload_bytes == 1024U * 1024U,
      "partial GPT report must count only the selected partition");

  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
  });
  check(verified.has_value(), "partial GPT image must reopen and verify");
  const auto& manifest = verified.value().manifest;
  const auto manifest_flags = static_cast<std::uint32_t>(manifest.flags);
  const auto selection_flag = static_cast<std::uint32_t>(
      ytec::imageformat::TsumugiManifestFlags::partition_selection);
  const auto selected_flag = static_cast<std::uint32_t>(
      ytec::imageformat::TsumugiManifestPartitionFlags::selected);
  check(
      manifest.partitions.size() == 2U &&
          (manifest_flags & selection_flag) != 0U &&
          (static_cast<std::uint32_t>(manifest.partitions[0].flags) &
           selected_flag) == 0U &&
          manifest.partitions[0].minimum_target_bytes == 0U &&
          manifest.partitions[0].planned_target_bytes == 0U &&
          manifest.partitions[0].payload_logical_offset == 0U &&
          manifest.partitions[0].payload_logical_length == 0U &&
          (static_cast<std::uint32_t>(manifest.partitions[1].flags) &
           selected_flag) != 0U &&
          manifest.partitions[1].source_partition_number == 2U &&
          manifest.partitions[1].payload_logical_offset ==
              2U * 1024U * 1024U &&
          manifest.partitions[1].payload_logical_length ==
              1024U * 1024U,
      "manifest must retain the full layout while binding payload only to partition 2");

  const bool read_unselected_esp_payload = std::any_of(
      state->reads->begin(),
      state->reads->end(),
      [](const auto& read) {
        constexpr std::uint64_t esp_begin = 1024U * 1024U;
        constexpr std::uint64_t esp_end = 1536U * 1024U;
        const auto read_end = read.first + read.second;
        return read.first < esp_end && read_end > esp_begin;
      });
  check(
      !read_unselected_esp_payload,
      "unselected ESP payload must never be read by the direct image stream");
}

void invalid_or_rescue_partition_selection_stops_before_environment_io() {
  TemporaryDirectory temporary;
  auto source = source_disk(ytec::diskmodel::PartitionStyle::gpt);
  for (const auto selection : {
           std::vector<std::uint32_t>{99U},
           std::vector<std::uint32_t>{2U, 2U}}) {
    auto state = std::make_shared<DependencyState>();
    auto request = request_for(
        source, temporary.image(L"invalid-partition-selection.tsumugi"));
    request.selected_partition_numbers = selection;
    const auto result = ytec::winpeapp::execute_direct_image_create(
        request, dependencies_for(source, gpt_storage(), state));
    check(
        !result.has_value() && state->read_only_calls == 0U &&
            state->guards.empty(),
        "unknown or duplicate partition selection must fail before environment I/O");
  }

  auto state = std::make_shared<DependencyState>();
  auto rescue = request_for(
      source, temporary.image(L"partial-rescue-not-supported.tsumugi"));
  rescue.rescue_mode = true;
  rescue.selected_partition_numbers = {2U};
  const auto result = ytec::winpeapp::execute_direct_image_create(
      rescue, dependencies_for(source, gpt_storage(), state));
  check(
      !result.has_value() && state->read_only_calls == 0U &&
          state->guards.empty(),
      "rescue mode must remain whole-disk and reject partial selection before I/O");
}

void mbr_rescue_image_uses_owned_staging_without_final_source_reread() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"mbr-rescue.tsumugi");
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  auto request = request_for(source, path);
  request.rescue_mode = true;
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request,
      dependencies_for(source, mbr_storage(), state, true));
  check(result.has_value(), "MBR rescue image must succeed");
  check(result.value().rescue_mode && result.value().rescue.has_value() &&
            !result.value().layout_revalidated_before_commit &&
            result.value().source_read_only_verified &&
            result.value().source_left_read_only,
        "Rescue result must avoid a final failing-source layout reread");
  check(!result.value().rescue->partial_data_loss &&
            result.value().rescue->target_flushed &&
            result.value().rescue->all_writes_read_back_verified &&
            result.value().image.complete_verification_passed &&
            result.value().image.stream.committed,
        "Lossless rescue must retain rescue classification and complete verification");
  check(state->guards.size() == 2U &&
            state->guards[1].required_available_bytes >
                kDiskSize + result.value().logical_payload_bytes &&
            state->rescue_staging->factory_calls == 1U &&
            state->rescue_staging->request.source_disk_size == kDiskSize &&
            state->rescue_staging->request.required_available_bytes ==
                state->guards[1].required_available_bytes &&
            state->rescue_staging->seal_calls == 1U &&
            state->rescue_staging->discard_calls == 1U &&
            state->rescue_staging->destination_validation_calls == 1U,
        "Rescue must reserve stage plus image and complete the owned lifecycle");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
  });
  check(verified.has_value() &&
            verified.value().manifest.mode ==
                ytec::imageformat::TsumugiManifestMode::rescue &&
            !verified.value().partial_loss,
        "Committed lossless rescue must reopen as rescue, not exact");
}

void same_physical_destination_stops_before_source_change() {
  TemporaryDirectory temporary;
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  auto dependencies = dependencies_for(source, mbr_storage(), state);
  dependencies.validate_destination =
      [state](const auto& guard) {
        state->guards.push_back(guard);
        return ytec::clonecore::Status::failure({
            .code = ytec::clonecore::ErrorCode::identity_mismatch,
            .native_code = ERROR_ACCESS_DENIED,
            .operation = L"合成保存先物理ディスク照合",
            .message = L"保存先はコピー元と同じ物理ディスクです",
        });
      };
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request_for(source, temporary.image(L"same-disk.tsumugi")),
      dependencies);
  check(!result.has_value() && state->read_only_calls == 0U &&
            state->guards.size() == 1U,
        "same-disk storage must fail before changing the source attribute");
}

void four_kn_rescue_stops_before_environment_io() {
  TemporaryDirectory temporary;
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  source.logical_sector_size = 4096U;
  source.physical_sector_size = 4096U;
  source.sector_count = source.size_bytes / source.logical_sector_size;
  auto state = std::make_shared<DependencyState>();
  auto request = request_for(
      source, temporary.image(L"unsupported-4kn-rescue.tsumugi"));
  request.rescue_mode = true;
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request, dependencies_for(source, mbr_storage(), state));
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::unsupported_layout &&
            state->read_only_calls == 0U && state->guards.empty() &&
            state->rescue_staging->factory_calls == 0U,
        "4Kn rescue image creation must fail before environment I/O");
}

void changed_layout_aborts_owned_partial_without_final_name() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"changed-layout.tsumugi");
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request_for(source, path),
      dependencies_for(source, mbr_storage(), state, true));
  check(!result.has_value(), "changed read-only layout must fail closed");
  check(!std::filesystem::exists(path) &&
            !std::filesystem::exists(path + L".partial"),
        "failed final layout recheck must remove only the owned partial");
}

void cancellation_before_start_performs_no_environment_call() {
  TemporaryDirectory temporary;
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  auto request = request_for(source, temporary.image(L"cancelled.tsumugi"));
  request.callbacks.cancellation_requested = [] { return true; };
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request, dependencies_for(source, mbr_storage(), state));
  check(!result.has_value() &&
            result.error().code == ytec::clonecore::ErrorCode::cancelled &&
            state->read_only_calls == 0U && state->guards.empty(),
        "pre-start cancellation must stop before every environment call");
}

void unknown_verification_mode_performs_no_environment_call() {
  TemporaryDirectory temporary;
  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  auto state = std::make_shared<DependencyState>();
  auto request = request_for(
      source, temporary.image(L"unknown-verification.tsumugi"));
  request.verification_mode = static_cast<
      ytec::imageformat::TsumugiCreateVerificationMode>(0xffU);
  const auto result = ytec::winpeapp::execute_direct_image_create(
      request, dependencies_for(source, mbr_storage(), state));
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::invalid_argument &&
            state->read_only_calls == 0U && state->guards.empty(),
        "an unknown PE verification mode must fail before environment I/O");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"mbr_exact_image_is_fully_verified_and_committed",
       mbr_exact_image_is_fully_verified_and_committed},
      {"mbr_fast_image_keeps_required_gates_without_complete_claim",
       mbr_fast_image_keeps_required_gates_without_complete_claim},
      {"gpt_exact_image_preserves_partition_roles",
       gpt_exact_image_preserves_partition_roles},
      {"gpt_partial_image_reads_only_selected_partition_payload",
       gpt_partial_image_reads_only_selected_partition_payload},
      {"invalid_or_rescue_partition_selection_stops_before_environment_io",
       invalid_or_rescue_partition_selection_stops_before_environment_io},
      {"mbr_rescue_image_uses_owned_staging_without_final_source_reread",
       mbr_rescue_image_uses_owned_staging_without_final_source_reread},
      {"same_physical_destination_stops_before_source_change",
       same_physical_destination_stops_before_source_change},
      {"four_kn_rescue_stops_before_environment_io",
       four_kn_rescue_stops_before_environment_io},
      {"changed_layout_aborts_owned_partial_without_final_name",
       changed_layout_aborts_owned_partial_without_final_name},
      {"cancellation_before_start_performs_no_environment_call",
       cancellation_before_start_performs_no_environment_call},
      {"unknown_verification_mode_performs_no_environment_call",
       unknown_verification_mode_performs_no_environment_call},
      {"resume_continuity_roundtrips_without_secret_material",
       resume_continuity_roundtrips_without_secret_material},
      {"resume_continuity_rejects_noncanonical_unknown_and_oversized_input",
       resume_continuity_rejects_noncanonical_unknown_and_oversized_input},
  };
  int failures = 0;
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

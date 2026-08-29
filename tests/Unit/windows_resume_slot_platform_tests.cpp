#include "ytec/operationcore/windows_resume_slot_platform.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

std::wstring extended_path(const std::wstring& path) {
  return L"\\\\?\\" + path;
}

void write_bytes(
    const std::wstring& path,
    const std::span<const std::byte> bytes) {
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(handle.valid(), "Synthetic file create must succeed");
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    DWORD written{};
    const DWORD amount = static_cast<DWORD>(bytes.size() - consumed);
    if (!WriteFile(
            handle.get(),
            bytes.data() + consumed,
            amount,
            &written,
            nullptr) ||
        written == 0U) {
      throw TestFailure("Synthetic file write must succeed");
    }
    consumed += written;
  }
  check(FlushFileBuffers(handle.get()) != FALSE,
        "Synthetic file flush must succeed");
}

std::vector<std::byte> read_bytes(const std::wstring& path) {
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(handle.valid(), "Synthetic file open must succeed");
  LARGE_INTEGER size{};
  check(GetFileSizeEx(handle.get(), &size) != FALSE && size.QuadPart >= 0,
        "Synthetic file size must be available");
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD read{};
  if (!bytes.empty()) {
    check(ReadFile(
              handle.get(),
              bytes.data(),
              static_cast<DWORD>(bytes.size()),
              &read,
              nullptr) != FALSE &&
              read == bytes.size(),
          "Synthetic file read must succeed");
  }
  return bytes;
}

void xor_file_byte(
    const std::wstring& path,
    const std::uint64_t offset,
    const std::byte mask) {
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!handle) {
    throw TestFailure("Synthetic tamper handle must open");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN)) {
    throw TestFailure("Synthetic tamper seek must succeed");
  }
  std::byte value{};
  DWORD transferred{};
  if (!ReadFile(handle.get(), &value, 1U, &transferred, nullptr) ||
      transferred != 1U) {
    throw TestFailure("Synthetic tamper read must succeed");
  }
  value ^= mask;
  if (!SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN) ||
      !WriteFile(handle.get(), &value, 1U, &transferred, nullptr) ||
      transferred != 1U || !FlushFileBuffers(handle.get())) {
    throw TestFailure("Synthetic tamper write and flush must succeed");
  }
}

void create_sized_file(
    const std::wstring& path,
    const std::uint64_t size) {
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(handle.valid(),
        "Synthetic oversized file create must succeed");
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(size);
  check(SetFilePointerEx(handle.get(), end, nullptr, FILE_BEGIN) != FALSE &&
            SetEndOfFile(handle.get()) != FALSE &&
            FlushFileBuffers(handle.get()) != FALSE,
        "Synthetic oversized file length must be set");
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1U> temp{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(temp.size()), temp.data());
    check(length != 0U && length < temp.size(),
          "Temporary path must be available");
    std::array<wchar_t, MAX_PATH + 1U> name{};
    check(GetTempFileNameW(temp.data(), L"yrs", 0U, name.data()) != 0U,
          "Temporary unique name must be available");
    check(DeleteFileW(name.data()) != FALSE,
          "Temporary placeholder must be removed");
    check(CreateDirectoryW(name.data(), nullptr) != FALSE,
          "Temporary root must be created");
    std::array<wchar_t, MAX_PATH + 1U> long_name{};
    const DWORD long_length = GetLongPathNameW(
        name.data(), long_name.data(), static_cast<DWORD>(long_name.size()));
    check(long_length != 0U && long_length < long_name.size(),
          "Temporary root must have a canonical long path");
    root_.assign(long_name.data(), long_length);
    application_ = root_ + L"\\app";
    data_ = application_ + L"\\data";
    output_ = root_ + L"\\output";
    executable_ = application_ + L"\\TsumugiDrive.exe";
    check(CreateDirectoryW(application_.c_str(), nullptr) != FALSE &&
              CreateDirectoryW(data_.c_str(), nullptr) != FALSE &&
              CreateDirectoryW(output_.c_str(), nullptr) != FALSE,
          "Synthetic directory tree must be created");
    constexpr std::array<std::byte, 4U> executable_marker{
        std::byte{0x4d}, std::byte{0x5a}, std::byte{0x00}, std::byte{0x00}};
    write_bytes(executable_, executable_marker);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
  [[nodiscard]] const std::wstring& data() const noexcept { return data_; }
  [[nodiscard]] const std::wstring& executable() const noexcept {
    return executable_;
  }
  [[nodiscard]] std::wstring checkpoint() const {
    return data_ + L"\\active.checkpoint";
  }
  [[nodiscard]] std::wstring stage() const {
    return data_ + L"\\active.checkpoint.new";
  }
  [[nodiscard]] std::wstring partial() const {
    return output_ + L"\\Tsumugi-Image.tsumugi.partial";
  }
  [[nodiscard]] std::wstring journal() const {
    return output_ + L"\\Tsumugi-Image.tsumugi.resume-journal.partial";
  }
  [[nodiscard]] std::wstring final_image() const {
    return output_ + L"\\Tsumugi-Image.tsumugi";
  }

 private:
  std::wstring root_;
  std::wstring application_;
  std::wstring data_;
  std::wstring output_;
  std::wstring executable_;
};

ytec::operationcore::Sha256Digest digest(const std::uint8_t seed) {
  ytec::operationcore::Sha256Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>((seed + index) & 0xffU);
  }
  return value;
}

ytec::operationcore::OperationId operation_id(const std::uint8_t seed) {
  ytec::operationcore::OperationId value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>((seed + index) & 0xffU);
  }
  return value;
}

ytec::clonecore::StableDiskIdentity target_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 11U,
      .model = L"Synthetic resume platform target",
      .size_bytes = 64ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = "RSM11",
      .device_instance_id = L"SYNTHETIC\\RESUME-PLATFORM-TARGET",
      .is_system_disk = false,
  };
}

ytec::operationcore::ParsedCheckpoint parsed_checkpoint(
    const std::uint64_t revision,
    const std::uint64_t verified_bytes,
    const std::uint64_t verified_chunks) {
  const ytec::operationcore::OperationPlan plan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = operation_id(0x10U),
      .kind = ytec::operationcore::OperationKind::image_restore,
      .environment = ytec::operationcore::OperationEnvironment::winpe,
      .source = std::nullopt,
      .target = target_identity(),
      .expected_work_bytes = 8192U,
      .immutable_payload_hash = digest(0x20U),
  };
  auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  check(plan_hash.has_value(), "Synthetic plan must hash");
  const ytec::operationcore::InterruptionCheckpoint checkpoint{
      .schema_version = ytec::operationcore::kCheckpointSchemaVersion,
      .operation_id = plan.operation_id,
      .kind = plan.kind,
      .environment = plan.environment,
      .phase = ytec::operationcore::CheckpointPhase::executing,
      .revision = revision,
      .expected_work_bytes = plan.expected_work_bytes,
      .verified_work_bytes = verified_bytes,
      .verified_chunk_count = verified_chunks,
      .plan_hash = plan_hash.value(),
      .output_identity_hash = digest(0x50U),
      .source = plan.source,
      .target = plan.target,
      .continuity_token = L"SYNTHETIC-RESUME-PLATFORM-EPOCH",
  };
  auto bytes = ytec::operationcore::serialize_checkpoint(checkpoint);
  check(bytes.has_value(), "Synthetic checkpoint must serialize");
  auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  check(parsed.has_value(), "Synthetic checkpoint must parse");
  return parsed.take_value();
}

ytec::operationcore::ParsedCheckpoint parse_checkpoint_value(
    const ytec::operationcore::InterruptionCheckpoint& checkpoint);

ytec::operationcore::ParsedCheckpoint parsed_image_checkpoint(
    const std::uint64_t revision,
    const ytec::operationcore::CheckpointPhase phase,
    const std::uint64_t verified_bytes,
    const std::uint64_t verified_chunks,
    const std::uint64_t primary_length,
    const std::uint64_t journal_length,
    const std::uint8_t prefix_seed) {
  auto source = target_identity();
  source.disk_number = 4U;
  source.model = L"Synthetic resume image source";
  source.serial_suffix = "IMG04";
  source.device_instance_id = L"SYNTHETIC\\RESUME-IMAGE-SOURCE";
  const ytec::operationcore::OperationPlan plan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = operation_id(0x61U),
      .kind = ytec::operationcore::OperationKind::image_create,
      .environment = ytec::operationcore::OperationEnvironment::winpe,
      .source = source,
      .target = std::nullopt,
      .expected_work_bytes = 8192U,
      .immutable_payload_hash = digest(0x62U),
  };
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  check(plan_hash.has_value(), "Synthetic image plan must hash");
  return parse_checkpoint_value({
      .schema_version = ytec::operationcore::kCheckpointSchemaVersionV3,
      .operation_id = plan.operation_id,
      .kind = plan.kind,
      .environment = plan.environment,
      .phase = phase,
      .revision = revision,
      .expected_work_bytes = plan.expected_work_bytes,
      .verified_work_bytes = verified_bytes,
      .verified_chunk_count = verified_chunks,
      .plan_hash = plan_hash.value(),
      .output_identity_hash = digest(0x50U),
      .source = plan.source,
      .target = std::nullopt,
      .continuity_token = L"PE-SOURCE-STATE-SYNTHETIC-0001",
      .preparation_evidence = std::nullopt,
      .output_progress_evidence =
          ytec::operationcore::CheckpointOutputProgressEvidence{
              .verified_prefix_hash = digest(prefix_seed),
              .primary_output_length = primary_length,
              .journal_length = journal_length,
              .auxiliary_output_length = 0U,
          },
  });
}

ytec::operationcore::ParsedCheckpoint parse_checkpoint_value(
    const ytec::operationcore::InterruptionCheckpoint& checkpoint) {
  auto bytes = ytec::operationcore::serialize_checkpoint(checkpoint);
  check(bytes.has_value(), "Synthetic checkpoint value must serialize");
  auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  check(parsed.has_value(), "Synthetic checkpoint value must parse");
  return parsed.take_value();
}

ytec::operationcore::ParsedCheckpoint maximum_preparation_checkpoint(
    const std::uint64_t revision,
    const ytec::operationcore::CheckpointPhase phase) {
  auto checkpoint = parsed_checkpoint(1U, 0U, 0U).checkpoint;
  checkpoint.schema_version =
      ytec::operationcore::kCheckpointSchemaVersionV2;
  checkpoint.phase = phase;
  checkpoint.revision = revision;
  checkpoint.preparation_evidence =
      ytec::operationcore::CheckpointPreparationEvidence{
          .initial_layout_hash = digest(0xa0U),
          .logical_sector_size = 512U,
      };
  auto& sectors =
      checkpoint.preparation_evidence->original_sectors;
  sectors.reserve(
      ytec::operationcore::kMaximumCheckpointPreparationSectors);
  constexpr std::uint64_t kSectorBytes = 512U;
  constexpr std::uint64_t kRangeBytes = 1024U * 1024U;
  constexpr std::uint64_t kTargetBytes = 64ULL * 1024ULL * 1024ULL;
  constexpr std::size_t kSectorsPerRange =
      static_cast<std::size_t>(kRangeBytes / kSectorBytes);
  for (std::size_t index = 0U; index < kSectorsPerRange; ++index) {
    sectors.push_back({
        .offset = static_cast<std::uint64_t>(index) * kSectorBytes,
        .length = static_cast<std::uint32_t>(kSectorBytes),
        .original_hash = digest(
            static_cast<std::uint8_t>(0x11U + (index % 0x80U))),
    });
  }
  for (std::size_t index = 0U; index < kSectorsPerRange; ++index) {
    sectors.push_back({
        .offset = kTargetBytes - kRangeBytes +
            static_cast<std::uint64_t>(index) * kSectorBytes,
        .length = static_cast<std::uint32_t>(kSectorBytes),
        .original_hash = digest(
            static_cast<std::uint8_t>(0x51U + (index % 0x80U))),
    });
  }
  check(
      sectors.size() ==
          ytec::operationcore::kMaximumCheckpointPreparationSectors,
      "Maximum checkpoint fixture must contain exactly 4096 sectors");
  return parse_checkpoint_value(checkpoint);
}

ytec::operationcore::ResumeIdentityBinding resume_identities() {
  return {
      .source_identity_hash = digest(0x30U),
      .target_identity_hash = digest(0x40U),
      .output_identity_hash = digest(0x50U),
  };
}

ytec::operationcore::ResumeSlotRecord record(
    const ytec::operationcore::ParsedCheckpoint& checkpoint,
    const std::optional<ytec::operationcore::ResumeOwnedPartialBinding>&
        partial = std::nullopt) {
  return {
      .capability =
          ytec::operationcore::ResumeCapability::persistent_exact_restore,
      .checkpoint = checkpoint,
      .identities = resume_identities(),
      .owned_partial = partial,
  };
}

ytec::operationcore::ResumeSlotRecord image_record(
    const ytec::operationcore::ParsedCheckpoint& checkpoint,
    const std::vector<ytec::operationcore::WindowsResumeOwnedObject>&
        objects) {
  std::vector<ytec::operationcore::ResumeOwnedObjectBinding> bindings;
  bindings.reserve(objects.size());
  for (const auto& object : objects) {
    bindings.push_back(object.binding);
  }
  return {
      .capability = ytec::operationcore::ResumeCapability::
          persistent_pe_exact_image_create,
      .checkpoint = checkpoint,
      .identities = resume_identities(),
      .owned_partial = std::nullopt,
      .owned_objects = std::move(bindings),
  };
}

struct BackingProof final {
  bool separated{true};
  bool from_handle{true};
  ytec::operationcore::Sha256Digest identity{digest(0x90U)};
  int calls{};

  ytec::clonecore::Result<
      ytec::operationcore::WindowsResumeDataBackingProof>
  operator()(
      const std::wstring& data_directory,
      const std::optional<ytec::operationcore::ResumeSlotRecord>&) {
    ++calls;
    check(data_directory.ends_with(L"\\app\\data"),
          "Proof must receive only the EXE-adjacent data directory");
    return ytec::clonecore::Result<
        ytec::operationcore::WindowsResumeDataBackingProof>::success({
        .backing_storage_identity_hash = identity,
        .identity_from_open_handle = from_handle,
        .separated_from_source = separated,
    });
  }
};

std::unique_ptr<ytec::operationcore::IResumeSlotPlatform> make_platform(
    const TemporaryDirectory& temporary,
    BackingProof& proof,
    std::optional<ytec::operationcore::WindowsResumeOwnedPartial> partial =
        std::nullopt,
    std::vector<ytec::operationcore::WindowsResumeOwnedObject> objects = {}) {
  auto platform = ytec::operationcore::make_windows_resume_slot_platform({
      .executable_path = temporary.executable(),
      .prove_data_backing_separation =
          [&proof](
              const std::wstring& data_directory,
              const std::optional<ytec::operationcore::ResumeSlotRecord>&
                  current) {
            return proof(data_directory, current);
          },
      .owned_partial_for_create = std::move(partial),
      .owned_objects_for_create = std::move(objects),
  });
  if (!platform) {
    std::wcerr << L"platform error: " << platform.error().operation << L" / "
               << platform.error().message << L" ("
               << platform.error().native_code << L")\n";
  }
  check(platform.has_value(), "Synthetic production platform must construct");
  return platform.take_value();
}

void test_restart_create_replace_and_discard() {
  TemporaryDirectory temporary;
  const auto first_record = record(parsed_checkpoint(1U, 1024U, 1U));
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto empty = slot.inspect();
    check(empty.has_value() && !empty.value(),
          "Fresh fixed slot must be empty");
    check(slot.create(first_record).has_value(),
          "CREATE_NEW plus readback must persist the first record");
  }
  check(GetFileAttributesW(temporary.checkpoint().c_str()) !=
            INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.stage().c_str()) ==
                INVALID_FILE_ATTRIBUTES,
        "Create must publish only active.checkpoint");

  ytec::operationcore::ResumeSlotRecord second_record;
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto restarted = slot.inspect();
    check(restarted.has_value() && restarted.value().has_value() &&
              restarted.value()->checkpoint.checkpoint.revision == 1U,
          "A new adapter instance must inspect the persisted record");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        *restarted.value());
    check(binding.has_value(), "Restarted record must bind");
    const auto next = parsed_checkpoint(2U, 2048U, 2U);
    check(slot.replace(binding.value(), next).has_value(),
          "Atomic replace must accept one monotonic checkpoint");
    second_record = record(next);
  }
  check(GetFileAttributesW(temporary.stage().c_str()) ==
            INVALID_FILE_ATTRIBUTES,
        "Atomic replace must not leave its owned stage");

  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto restarted = slot.inspect();
    check(restarted.has_value() && restarted.value().has_value() &&
              restarted.value()->checkpoint.checkpoint.revision == 2U,
          "The atomically replaced revision must survive restart");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        second_record);
    check(binding.has_value() && slot.discard(binding.value()).has_value(),
          "Binding-checked discard must remove the persisted checkpoint");
  }
  check(GetFileAttributesW(temporary.checkpoint().c_str()) ==
            INVALID_FILE_ATTRIBUTES &&
            proof.calls >= 6,
        "Discard must leave no checkpoint and must repeatedly prove backing");
}

void test_maximum_v2_checkpoint_envelope_survives_restart_and_replace() {
  TemporaryDirectory temporary;
  const auto preparing = maximum_preparation_checkpoint(
      1U, ytec::operationcore::CheckpointPhase::preparing);
  const auto first_record = record(preparing);
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(first_record).has_value(),
          "The maximum v2 preparation checkpoint must persist");
  }
  const auto envelope = read_bytes(temporary.checkpoint());
  check(
      envelope.size() > 128U * 1024U &&
          envelope.size() <=
              ytec::operationcore::kMaximumWindowsResumeSlotBytes,
      "The real slot envelope must cross the legacy 128 KiB boundary and remain bounded");

  const auto prepared = maximum_preparation_checkpoint(
      2U, ytec::operationcore::CheckpointPhase::prepared);
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto inspected = slot.inspect();
    check(
        inspected && inspected.value() &&
            inspected.value()
                    ->checkpoint.checkpoint.preparation_evidence
                    ->original_sectors.size() ==
                ytec::operationcore::kMaximumCheckpointPreparationSectors,
        "Restart inspection must parse all 4096 authenticated sector digests");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        *inspected.value());
    check(binding && slot.replace(binding.value(), prepared).has_value(),
          "Atomic replace must accept the maximum v2 phase transition");
  }
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto inspected = slot.inspect();
    check(
        inspected && inspected.value() &&
            inspected.value()->checkpoint.checkpoint.phase ==
                ytec::operationcore::CheckpointPhase::prepared &&
            inspected.value()->checkpoint.checkpoint.revision == 2U,
        "The maximum replaced envelope must remain inspectable after another restart");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        *inspected.value());
    check(binding && slot.discard(binding.value()).has_value(),
          "The maximum envelope must remain safely discardable");
  }
}

void test_schema_v1_slot_envelope_remains_restart_compatible() {
  TemporaryDirectory temporary;
  auto legacy_checkpoint = parsed_checkpoint(1U, 1024U, 1U).checkpoint;
  legacy_checkpoint.schema_version =
      ytec::operationcore::kCheckpointSchemaVersionV1;
  legacy_checkpoint.phase =
      ytec::operationcore::CheckpointPhase::executing;
  legacy_checkpoint.preparation_evidence.reset();
  const auto legacy = parse_checkpoint_value(legacy_checkpoint);
  const auto legacy_record = record(legacy);
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(legacy_record).has_value(),
          "A schema-v1 checkpoint must persist in the current envelope");
  }
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto inspected = slot.inspect();
    check(
        inspected && inspected.value() &&
            inspected.value()->checkpoint.checkpoint.schema_version ==
                ytec::operationcore::kCheckpointSchemaVersionV1 &&
            !inspected.value()
                 ->checkpoint.checkpoint.preparation_evidence,
        "A restarted platform must parse legacy schema-v1 payloads without inventing evidence");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        *inspected.value());
    check(binding && slot.discard(binding.value()).has_value(),
          "A legacy slot must remain safely discardable");
  }
}

void test_owned_partial_survives_restart_and_discards_as_a_pair() {
  TemporaryDirectory temporary;
  std::vector<std::byte> payload(4096U, std::byte{0x5a});
  write_bytes(temporary.partial(), payload);
  const auto identities = resume_identities();
  auto partial = ytec::operationcore::bind_windows_resume_owned_partial(
      temporary.partial(), operation_id(0x10U), identities);
  check(partial.has_value(), "A regular single-link partial must bind");
  const auto first_record =
      record(parsed_checkpoint(1U, 1024U, 1U), partial.value().binding);
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof, partial.value());
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(first_record).has_value(),
          "A matching opened partial may be attached to CREATE_NEW");
  }

  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto restarted = slot.inspect();
    check(restarted.has_value() && restarted.value().has_value() &&
              restarted.value()->owned_partial.has_value(),
          "Persisted partial path and binding must reopen after restart");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        *restarted.value());
    check(binding.has_value() && slot.discard(binding.value()).has_value(),
          "Guarded pair discard must remove exact partial and checkpoint");
  }
  check(GetFileAttributesW(temporary.partial().c_str()) ==
            INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.checkpoint().c_str()) ==
                INVALID_FILE_ATTRIBUTES,
        "Pair discard must leave neither owned object");
}

void test_checkpoint_tamper_is_bounded_and_never_discarded() {
  TemporaryDirectory temporary;
  const auto first_record = record(parsed_checkpoint(1U, 1024U, 1U));
  auto binding = ytec::operationcore::make_resume_slot_binding(first_record);
  check(binding.has_value(), "Synthetic record must bind");
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(first_record).has_value(),
          "Tamper fixture must create a valid slot");
  }
  const auto bytes = read_bytes(temporary.checkpoint());
  check(bytes.size() > 20U, "Persisted envelope must have authenticated bytes");
  xor_file_byte(temporary.checkpoint(), 20U, std::byte{0x01});

  auto platform = make_platform(temporary, proof);
  ytec::operationcore::SingleResumeSlot slot(*platform);
  check(!slot.inspect() && !slot.discard(binding.value()) &&
            GetFileAttributesW(temporary.checkpoint().c_str()) !=
                INVALID_FILE_ATTRIBUTES,
        "Tampered envelope must fail closed and remain untouched");
}

void test_replaced_partial_is_never_deleted() {
  TemporaryDirectory temporary;
  std::vector<std::byte> original(1024U, std::byte{0x11});
  write_bytes(temporary.partial(), original);
  auto partial = ytec::operationcore::bind_windows_resume_owned_partial(
      temporary.partial(), operation_id(0x10U), resume_identities());
  check(partial.has_value(), "Original partial must bind");
  const auto first_record =
      record(parsed_checkpoint(1U, 1024U, 1U), partial.value().binding);
  auto binding = ytec::operationcore::make_resume_slot_binding(first_record);
  check(binding.has_value(), "Partial record must bind");
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof, partial.value());
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(first_record).has_value(),
          "Partial tamper fixture must persist");
  }
  check(DeleteFileW(temporary.partial().c_str()) != FALSE,
        "Original partial must be replaceable after simulated crash");
  std::vector<std::byte> replacement(1024U, std::byte{0x22});
  write_bytes(temporary.partial(), replacement);

  auto platform = make_platform(temporary, proof);
  ytec::operationcore::SingleResumeSlot slot(*platform);
  check(!slot.inspect() && !slot.discard(binding.value()) &&
            read_bytes(temporary.partial()) == replacement &&
            GetFileAttributesW(temporary.checkpoint().c_str()) !=
                INVALID_FILE_ATTRIBUTES,
        "File-ID mismatch must preserve replacement partial and checkpoint");
}

void test_hardlinks_are_rejected_for_checkpoint_and_partial() {
  {
    TemporaryDirectory temporary;
    const auto first_record = record(parsed_checkpoint(1U, 1024U, 1U));
    BackingProof proof;
    {
      auto platform = make_platform(temporary, proof);
      ytec::operationcore::SingleResumeSlot slot(*platform);
      check(slot.create(first_record).has_value(),
            "Checkpoint hardlink fixture must persist");
    }
    const std::wstring link = temporary.data() + L"\\checkpoint-link.bin";
    check(CreateHardLinkW(link.c_str(), temporary.checkpoint().c_str(), nullptr) !=
              FALSE,
          "Synthetic checkpoint hardlink must be created");
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(!slot.inspect() &&
              GetFileAttributesW(temporary.checkpoint().c_str()) !=
                  INVALID_FILE_ATTRIBUTES,
          "A multi-link checkpoint must fail closed without deletion");
  }

  {
    TemporaryDirectory temporary;
    std::vector<std::byte> payload(256U, std::byte{0x33});
    write_bytes(temporary.partial(), payload);
    auto partial = ytec::operationcore::bind_windows_resume_owned_partial(
        temporary.partial(), operation_id(0x10U), resume_identities());
    check(partial.has_value(), "Partial must bind before hardlink tamper");
    const std::wstring link = temporary.root() + L"\\partial-link.bin";
    check(CreateHardLinkW(link.c_str(), temporary.partial().c_str(), nullptr) !=
              FALSE,
          "Synthetic partial hardlink must be created");
    BackingProof proof;
    auto platform = ytec::operationcore::make_windows_resume_slot_platform({
        .executable_path = temporary.executable(),
        .prove_data_backing_separation =
            [&proof](
                const std::wstring& data,
                const std::optional<ytec::operationcore::ResumeSlotRecord>&
                    current) { return proof(data, current); },
        .owned_partial_for_create = partial.value(),
    });
    check(!platform,
          "A create candidate changed to multiple hardlinks must be rejected");
  }
}

#pragma pack(push, 1)
struct MountPointReparseData final {
  DWORD reparse_tag{};
  WORD reparse_data_length{};
  WORD reserved{};
  WORD substitute_name_offset{};
  WORD substitute_name_length{};
  WORD print_name_offset{};
  WORD print_name_length{};
  wchar_t path_buffer[1]{};
};
#pragma pack(pop)

void create_directory_junction(
    const std::wstring& junction,
    const std::wstring& target) {
  check(CreateDirectoryW(junction.c_str(), nullptr) != FALSE,
        "Junction directory must be created");
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      junction.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr));
  check(handle.valid(),
        "Junction directory handle must open");
  const std::wstring substitute = L"\\??\\" + target;
  const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
  const std::size_t print_bytes = target.size() * sizeof(wchar_t);
  const std::size_t path_bytes =
      substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
  const std::size_t allocation =
      offsetof(MountPointReparseData, path_buffer) + path_bytes;
  std::vector<std::byte> storage(allocation, std::byte{0});
  auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
  data->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
  data->substitute_name_offset = 0U;
  data->substitute_name_length = static_cast<WORD>(substitute_bytes);
  data->print_name_offset =
      static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
  data->print_name_length = static_cast<WORD>(print_bytes);
  data->reparse_data_length = static_cast<WORD>(
      sizeof(data->substitute_name_offset) +
      sizeof(data->substitute_name_length) +
      sizeof(data->print_name_offset) +
      sizeof(data->print_name_length) + path_bytes);
  std::memcpy(data->path_buffer, substitute.data(), substitute_bytes);
  std::memcpy(
      reinterpret_cast<std::byte*>(data->path_buffer) +
          data->print_name_offset,
      target.data(),
      print_bytes);
  DWORD returned{};
  const BOOL created = DeviceIoControl(
      handle.get(),
      FSCTL_SET_REPARSE_POINT,
      data,
      static_cast<DWORD>(
          offsetof(MountPointReparseData, path_buffer) + path_bytes),
      nullptr,
      0U,
      &returned,
      nullptr);
  const DWORD native_code = created ? ERROR_SUCCESS : GetLastError();
  check(created != FALSE,
        "Synthetic junction creation failed with code " +
            std::to_string(native_code));
}

void test_reparse_data_directory_is_rejected() {
  TemporaryDirectory temporary;
  const std::wstring real_data = temporary.root() + L"\\real-data";
  check(RemoveDirectoryW(temporary.data().c_str()) != FALSE &&
            CreateDirectoryW(real_data.c_str(), nullptr) != FALSE,
        "Data directory must be replaced by a synthetic junction");
  create_directory_junction(temporary.data(), real_data);
  BackingProof proof;
  auto platform = ytec::operationcore::make_windows_resume_slot_platform({
      .executable_path = temporary.executable(),
      .prove_data_backing_separation =
          [&proof](
              const std::wstring& data,
              const std::optional<ytec::operationcore::ResumeSlotRecord>&
                  current) { return proof(data, current); },
      .owned_partial_for_create = std::nullopt,
  });
  check(!platform,
        "An EXE-adjacent data reparse point must fail platform construction");
  check(RemoveDirectoryW(temporary.data().c_str()) != FALSE,
        "Synthetic junction must be removed without traversing its target");
}

void test_oversize_and_unproven_backing_fail_closed() {
  {
    TemporaryDirectory temporary;
    create_sized_file(
        temporary.checkpoint(),
        ytec::operationcore::kMaximumWindowsResumeSlotBytes + 1U);
    BackingProof proof;
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(!slot.inspect() &&
              GetFileAttributesW(temporary.checkpoint().c_str()) !=
                  INVALID_FILE_ATTRIBUTES,
          "Oversized slot must be rejected before allocation and preserved");
  }

  {
    TemporaryDirectory temporary;
    BackingProof proof;
    proof.separated = false;
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto inspected = slot.inspect();
    check(inspected.has_value() && !inspected.value() &&
              GetFileAttributesW(temporary.checkpoint().c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
          "Startup inspection without a selected source must need only an opened data-backing identity");
    check(!slot.create(record(parsed_checkpoint(1U, 1024U, 1U))) &&
              GetFileAttributesW(temporary.checkpoint().c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
          "Unproven source/data backing separation must block slot creation");
  }

  {
    TemporaryDirectory temporary;
    const auto owned_record = record(parsed_checkpoint(1U, 1024U, 1U));
    BackingProof creation_proof;
    {
      auto platform = make_platform(temporary, creation_proof);
      ytec::operationcore::SingleResumeSlot slot(*platform);
      check(slot.create(owned_record).has_value(),
            "Discard-without-source fixture must create its owned slot");
    }
    BackingProof startup_proof;
    startup_proof.separated = false;
    auto platform = make_platform(temporary, startup_proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    auto inspected = slot.inspect();
    check(inspected && inspected.value().has_value(),
          "Restart inspection must not require image/source reselection");
    auto binding = ytec::operationcore::make_resume_slot_binding(
        *inspected.value());
    check(binding && slot.discard(binding.value()).has_value() &&
              GetFileAttributesW(temporary.checkpoint().c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
          "Binding-checked owned discard must complete without source reselection");
  }
}

void test_stage_collision_and_locked_discard_preserve_owned_state() {
  TemporaryDirectory temporary;
  constexpr std::array<std::byte, 6U> foreign_stage{
      std::byte{0x66}, std::byte{0x6f}, std::byte{0x72},
      std::byte{0x65}, std::byte{0x69}, std::byte{0x67}};
  write_bytes(temporary.stage(), foreign_stage);
  const auto first_record = record(parsed_checkpoint(1U, 1024U, 1U));
  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(!slot.create(first_record) &&
              read_bytes(temporary.stage()) ==
                  std::vector<std::byte>(
                      foreign_stage.begin(), foreign_stage.end()) &&
              GetFileAttributesW(temporary.checkpoint().c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
          "CREATE_NEW stage collision must preserve the foreign stage");
  }
  check(DeleteFileW(temporary.stage().c_str()) != FALSE,
        "Foreign stage fixture must be removed explicitly");

  auto platform = make_platform(temporary, proof);
  ytec::operationcore::SingleResumeSlot slot(*platform);
  check(slot.create(first_record).has_value(),
        "Locked discard fixture must create a slot");
  auto binding = ytec::operationcore::make_resume_slot_binding(first_record);
  check(binding.has_value(), "Locked discard fixture must bind");
  ytec::clonecore::UniqueHandle blocker(CreateFileW(
      temporary.checkpoint().c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(blocker.valid(),
        "Synthetic non-delete-sharing blocker must open");
  check(!slot.discard(binding.value()) &&
            GetFileAttributesW(temporary.checkpoint().c_str()) !=
                INVALID_FILE_ATTRIBUTES,
        "A locked checkpoint must make discard fail without deletion");
  blocker.reset();
  check(slot.discard(binding.value()).has_value(),
        "Discard must succeed after the blocker is gone");
}

void test_locked_partial_rolls_back_checkpoint_discard() {
  TemporaryDirectory temporary;
  std::vector<std::byte> payload(1024U, std::byte{0x44});
  write_bytes(temporary.partial(), payload);
  auto partial = ytec::operationcore::bind_windows_resume_owned_partial(
      temporary.partial(), operation_id(0x10U), resume_identities());
  check(partial.has_value(), "Rollback fixture partial must bind");
  const auto first_record =
      record(parsed_checkpoint(1U, 1024U, 1U), partial.value().binding);
  auto binding = ytec::operationcore::make_resume_slot_binding(first_record);
  check(binding.has_value(), "Rollback fixture record must bind");
  BackingProof proof;
  auto platform = make_platform(temporary, proof, partial.value());
  ytec::operationcore::SingleResumeSlot slot(*platform);
  check(slot.create(first_record).has_value(),
        "Rollback fixture must persist both owned objects");

  ytec::clonecore::UniqueHandle blocker(CreateFileW(
      temporary.partial().c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(blocker.valid(),
        "Synthetic partial non-delete-sharing blocker must open");
  check(!slot.discard(binding.value()) &&
            GetFileAttributesW(temporary.checkpoint().c_str()) !=
                INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.partial().c_str()) !=
                INVALID_FILE_ATTRIBUTES,
        "Partial delete failure must roll checkpoint delete-pending back");
  blocker.reset();
  auto restarted = slot.inspect();
  check(restarted.has_value() && restarted.value().has_value() &&
            slot.discard(binding.value()).has_value(),
        "Rolled-back pair must remain inspectable and discardable");
}

void test_replace_stage_failure_keeps_current_revision() {
  TemporaryDirectory temporary;
  const auto first_record = record(parsed_checkpoint(1U, 1024U, 1U));
  BackingProof proof;
  auto platform = make_platform(temporary, proof);
  ytec::operationcore::SingleResumeSlot slot(*platform);
  check(slot.create(first_record).has_value(),
        "Replace failure fixture must create revision one");
  auto binding = ytec::operationcore::make_resume_slot_binding(first_record);
  check(binding.has_value(), "Replace failure fixture must bind");
  constexpr std::array<std::byte, 4U> collision{
      std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef}};
  write_bytes(temporary.stage(), collision);
  check(!slot.replace(
              binding.value(), parsed_checkpoint(2U, 2048U, 2U)) &&
            read_bytes(temporary.stage()) ==
                std::vector<std::byte>(collision.begin(), collision.end()),
        "Replace CREATE_NEW failure must preserve foreign stage");
  check(DeleteFileW(temporary.stage().c_str()) != FALSE,
        "Replace collision fixture must be removed explicitly");
  auto current = slot.inspect();
  check(current.has_value() && current.value().has_value() &&
            current.value()->checkpoint.checkpoint.revision == 1U,
        "Failed replace must leave the current slot revision intact");
}

void test_v3_multi_object_slot_survives_restart_and_discards_exact_set() {
  TemporaryDirectory temporary;
  constexpr std::array<std::byte, 4U> partial_marker{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  constexpr std::array<std::byte, 4U> journal_marker{
      std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x80}};
  write_bytes(temporary.partial(), partial_marker);
  write_bytes(temporary.journal(), journal_marker);

  const auto identities = resume_identities();
  const auto partial = ytec::operationcore::bind_windows_resume_owned_object(
      temporary.partial(),
      ytec::operationcore::ResumeOwnedObjectRole::image_partial,
      operation_id(0x61U),
      identities);
  const auto journal = ytec::operationcore::bind_windows_resume_owned_object(
      temporary.journal(),
      ytec::operationcore::ResumeOwnedObjectRole::image_resume_journal,
      operation_id(0x61U),
      identities);
  check(partial.has_value() && journal.has_value(),
        "Both v3 owned file objects must bind from opened handles");
  const std::vector<ytec::operationcore::WindowsResumeOwnedObject> objects{
      partial.value(), journal.value()};
  auto first = image_record(
      parsed_image_checkpoint(
          1U,
          ytec::operationcore::CheckpointPhase::preparing,
          0U,
          0U,
          0U,
          0U,
          0x64U),
      objects);

  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof, std::nullopt, objects);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(first).has_value(),
          "A v3 checkpoint must persist both authenticated object paths");
  }

  auto platform = make_platform(temporary, proof);
  ytec::operationcore::SingleResumeSlot slot(*platform);
  auto restarted = slot.inspect();
  check(restarted.has_value() && restarted.value() &&
            restarted.value()->owned_objects.size() == 2U,
        "Restart must re-open and prove both persisted File IDs");
  const auto binding =
      ytec::operationcore::make_resume_slot_binding(*restarted.value());
  check(binding.has_value(), "Restarted v3 slot must produce exact binding");

  auto second = image_record(
      parsed_image_checkpoint(
          2U,
          ytec::operationcore::CheckpointPhase::prepared,
          0U,
          0U,
          0U,
          4U,
          0x65U),
      objects);
  check(slot.replace(binding.value(), second.checkpoint).has_value(),
        "V3 replace must preserve both immutable object bindings");
  const auto second_binding =
      ytec::operationcore::make_resume_slot_binding(second);
  check(second_binding.has_value() &&
            slot.discard(second_binding.value()).has_value(),
        "Exact v3 discard must remove checkpoint, partial, and journal");
  check(GetFileAttributesW(temporary.checkpoint().c_str()) ==
            INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.partial().c_str()) ==
                INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.journal().c_str()) ==
                INVALID_FILE_ATTRIBUTES,
        "No member of the exact owned set may survive successful discard");
}

void test_v3_missing_or_relinked_object_is_never_adopted_or_deleted() {
  TemporaryDirectory temporary;
  const std::vector<std::byte> partial_marker{
      std::byte{0x11}, std::byte{0x21}, std::byte{0x31}, std::byte{0x41}};
  const std::vector<std::byte> journal_marker{
      std::byte{0x51}, std::byte{0x61}, std::byte{0x71}, std::byte{0x81}};
  const std::vector<std::byte> replacement_marker{
      std::byte{0x91}, std::byte{0xA1}, std::byte{0xB1}, std::byte{0xC1}};
  write_bytes(temporary.partial(), partial_marker);
  write_bytes(temporary.journal(), journal_marker);

  const auto identities = resume_identities();
  const auto partial = ytec::operationcore::bind_windows_resume_owned_object(
      temporary.partial(),
      ytec::operationcore::ResumeOwnedObjectRole::image_partial,
      operation_id(0x61U),
      identities);
  const auto journal = ytec::operationcore::bind_windows_resume_owned_object(
      temporary.journal(),
      ytec::operationcore::ResumeOwnedObjectRole::image_resume_journal,
      operation_id(0x61U),
      identities);
  check(partial.has_value() && journal.has_value(),
        "V3 orphan/relink fixture objects must bind");
  const std::vector<ytec::operationcore::WindowsResumeOwnedObject> objects{
      partial.value(), journal.value()};
  const auto record = image_record(
      parsed_image_checkpoint(
          1U,
          ytec::operationcore::CheckpointPhase::preparing,
          0U,
          0U,
          0U,
          0U,
          0x66U),
      objects);
  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value(), "V3 orphan/relink fixture must bind its slot");

  BackingProof proof;
  {
    auto platform = make_platform(temporary, proof, std::nullopt, objects);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(record).has_value(),
          "V3 orphan/relink fixture must persist");
  }

  check(DeleteFileW(temporary.journal().c_str()) != FALSE,
        "Owned journal must be removable after the simulated crash");
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(!slot.inspect() && !slot.discard(binding.value()) &&
              GetFileAttributesW(temporary.checkpoint().c_str()) !=
                  INVALID_FILE_ATTRIBUTES &&
              read_bytes(temporary.partial()) == partial_marker &&
              GetFileAttributesW(temporary.journal().c_str()) ==
                  INVALID_FILE_ATTRIBUTES,
          "A missing owned journal must fail closed without deleting the remaining exact set");
  }

  write_bytes(temporary.journal(), replacement_marker);
  {
    auto platform = make_platform(temporary, proof);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(!slot.inspect() && !slot.discard(binding.value()) &&
              GetFileAttributesW(temporary.checkpoint().c_str()) !=
                  INVALID_FILE_ATTRIBUTES &&
              read_bytes(temporary.partial()) == partial_marker &&
              read_bytes(temporary.journal()) == replacement_marker,
          "A relinked journal must never be adopted or deleted through the stale binding");
  }
}

struct ImageCommitFixture final {
  ytec::operationcore::ResumeSlotBinding binding;
  std::vector<std::byte> image_marker;
  std::vector<std::byte> journal_marker;
  ytec::operationcore::Sha256Digest global_hash{};
};

ImageCommitFixture create_image_commit_fixture(
    const TemporaryDirectory& temporary,
    BackingProof& proof) {
  ImageCommitFixture fixture{
      .image_marker = {
          std::byte{0x14}, std::byte{0x24}, std::byte{0x34}, std::byte{0x44}},
      .journal_marker = {
          std::byte{0x54}, std::byte{0x64}, std::byte{0x74}, std::byte{0x84}},
      .global_hash = digest(0x70U),
  };
  write_bytes(temporary.partial(), fixture.image_marker);
  write_bytes(temporary.journal(), fixture.journal_marker);
  const auto identities = resume_identities();
  auto partial = ytec::operationcore::bind_windows_resume_owned_object(
      temporary.partial(),
      ytec::operationcore::ResumeOwnedObjectRole::image_partial,
      operation_id(0x61U),
      identities);
  auto journal = ytec::operationcore::bind_windows_resume_owned_object(
      temporary.journal(),
      ytec::operationcore::ResumeOwnedObjectRole::image_resume_journal,
      operation_id(0x61U),
      identities);
  check(partial && journal,
        "Commit-ready image partial and journal must bind");
  const std::vector<ytec::operationcore::WindowsResumeOwnedObject> objects{
      partial.value(), journal.value()};
  const auto record = image_record(
      parsed_image_checkpoint(
          1U,
          ytec::operationcore::CheckpointPhase::commit_ready,
          8192U,
          2U,
          fixture.image_marker.size(),
          fixture.journal_marker.size(),
          0x70U),
      objects);
  auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value(), "Commit-ready image slot must bind");
  {
    auto platform = make_platform(
        temporary, proof, std::nullopt, objects);
    ytec::operationcore::SingleResumeSlot slot(*platform);
    check(slot.create(record).has_value(),
          "Commit-ready image slot must persist before crash window tests");
  }
  fixture.binding = binding.take_value();
  return fixture;
}

ytec::operationcore::PersistentPeExactImageCreateCommitRequest
image_commit_request(
    const TemporaryDirectory& temporary,
    const ImageCommitFixture& fixture,
    int& reproof_calls,
    int& verifier_calls) {
  return {
      .reviewed_binding = fixture.binding,
      .reviewed_final_path = temporary.final_image(),
      .reprove_before_publish = [&reproof_calls]() {
        ++reproof_calls;
        return ytec::clonecore::success_status();
      },
      .verify_published_image =
          [&fixture, &verifier_calls](const std::wstring&) {
            ++verifier_calls;
            return ytec::clonecore::Result<ytec::operationcore::
                PersistentPeExactImageCreateVerification>::success({
                .image_length = fixture.image_marker.size(),
                .global_hash = fixture.global_hash,
                .header_hash_verified = true,
                .metadata_authenticated = true,
                .all_chunks_verified = true,
                .global_hash_verified = true,
            });
          },
  };
}

void test_image_commit_publishes_absent_final_then_retires_exact_set() {
  TemporaryDirectory temporary;
  BackingProof proof;
  const auto fixture = create_image_commit_fixture(temporary, proof);
  auto platform = make_platform(temporary, proof);
  auto inspected = platform->inspect_persistent_pe_exact_image_create();
  check(inspected && inspected.value().state == ytec::operationcore::
              PersistentPeExactImageCreateObjectState::staged &&
            inspected.value().final_path == temporary.final_image() &&
            inspected.value().final_path_available,
        "Commit-ready partial must classify as staged with an absent final");
  int reproof_calls{};
  int verifier_calls{};
  auto committed = platform->commit_persistent_pe_exact_image_create(
      image_commit_request(
          temporary, fixture, reproof_calls, verifier_calls));
  if (!committed) {
    std::wcerr << L"staged commit error: " << committed.error().operation
               << L" / " << committed.error().message << L" ("
               << committed.error().native_code << L")\n";
  }
  check(committed && committed.value().image_published &&
            committed.value().complete_image_verified &&
            committed.value().journal_retired &&
            committed.value().slot_retired &&
            !committed.value().recovered_after_publish &&
            reproof_calls == 1 && verifier_calls == 1 &&
            read_bytes(temporary.final_image()) == fixture.image_marker &&
            GetFileAttributesW(temporary.partial().c_str()) ==
                INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.journal().c_str()) ==
                INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.checkpoint().c_str()) ==
                INVALID_FILE_ATTRIBUTES,
        "Staged commit must no-overwrite publish, verify, then retire journal and slot");
}

void test_image_commit_never_overwrites_an_existing_final() {
  TemporaryDirectory temporary;
  BackingProof proof;
  const auto fixture = create_image_commit_fixture(temporary, proof);
  const std::vector<std::byte> existing{
      std::byte{0x91}, std::byte{0x92}, std::byte{0x93}};
  write_bytes(temporary.final_image(), existing);
  auto platform = make_platform(temporary, proof);
  auto inspected = platform->inspect_persistent_pe_exact_image_create();
  check(inspected && inspected.value().state == ytec::operationcore::
              PersistentPeExactImageCreateObjectState::staged &&
            !inspected.value().final_path_available,
        "An unrelated completed name must not be adopted as the staged output");
  int reproof_calls{};
  int verifier_calls{};
  auto committed = platform->commit_persistent_pe_exact_image_create(
      image_commit_request(
          temporary, fixture, reproof_calls, verifier_calls));
  check(!committed && reproof_calls == 0 && verifier_calls == 0 &&
            read_bytes(temporary.final_image()) == existing &&
            read_bytes(temporary.partial()) == fixture.image_marker &&
            read_bytes(temporary.journal()) == fixture.journal_marker &&
            GetFileAttributesW(temporary.checkpoint().c_str()) !=
                INVALID_FILE_ATTRIBUTES,
        "Existing final must block publish without touching either transaction");
}

void test_image_commit_recovers_after_publish_crash_without_source_reproof() {
  TemporaryDirectory temporary;
  BackingProof proof;
  const auto fixture = create_image_commit_fixture(temporary, proof);
  check(MoveFileExW(
            temporary.partial().c_str(),
            temporary.final_image().c_str(),
            0U) != FALSE,
        "Synthetic crash must move the same File ID before journal retirement");
  auto platform = make_platform(temporary, proof);
  auto inspected = platform->inspect_persistent_pe_exact_image_create();
  check(inspected && inspected.value().state == ytec::operationcore::
              PersistentPeExactImageCreateObjectState::published,
        "Same pre-move File ID under final name must classify as published");
  int reproof_calls{};
  int verifier_calls{};
  auto request = image_commit_request(
      temporary, fixture, reproof_calls, verifier_calls);
  request.reprove_before_publish = {};
  auto committed = platform->commit_persistent_pe_exact_image_create(request);
  check(committed && committed.value().recovered_after_publish &&
            committed.value().image_published &&
            committed.value().complete_image_verified &&
            committed.value().journal_retired &&
            committed.value().slot_retired && reproof_calls == 0 &&
            verifier_calls == 1 &&
            read_bytes(temporary.final_image()) == fixture.image_marker &&
            GetFileAttributesW(temporary.journal().c_str()) ==
                INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(temporary.checkpoint().c_str()) ==
                INVALID_FILE_ATTRIBUTES,
        "Post-publish recovery must verify the exact final File ID and retire without source I/O");
}

void test_image_commit_recovers_after_journal_retire_crash() {
  TemporaryDirectory temporary;
  BackingProof proof;
  const auto fixture = create_image_commit_fixture(temporary, proof);
  check(MoveFileExW(
            temporary.partial().c_str(),
            temporary.final_image().c_str(),
            0U) != FALSE &&
            DeleteFileW(temporary.journal().c_str()) != FALSE,
        "Synthetic retirement crash must preserve final and checkpoint only");
  auto platform = make_platform(temporary, proof);
  auto inspected = platform->inspect_persistent_pe_exact_image_create();
  check(inspected && inspected.value().state == ytec::operationcore::
              PersistentPeExactImageCreateObjectState::retirement_pending,
        "Missing exact journal after same-File-ID publish must classify as retirement pending");
  int reproof_calls{};
  int verifier_calls{};
  auto request = image_commit_request(
      temporary, fixture, reproof_calls, verifier_calls);
  request.reprove_before_publish = {};
  auto committed = platform->commit_persistent_pe_exact_image_create(request);
  check(committed && committed.value().recovered_after_publish &&
            committed.value().complete_image_verified &&
            committed.value().journal_retired &&
            committed.value().slot_retired && verifier_calls == 1 &&
            read_bytes(temporary.final_image()) == fixture.image_marker &&
            GetFileAttributesW(temporary.checkpoint().c_str()) ==
                INVALID_FILE_ATTRIBUTES,
        "Retirement-pending recovery must reverify final then retire only the checkpoint");
}

void test_image_post_publish_relinked_final_fails_closed() {
  TemporaryDirectory temporary;
  BackingProof proof;
  const auto fixture = create_image_commit_fixture(temporary, proof);
  check(MoveFileExW(
            temporary.partial().c_str(),
            temporary.final_image().c_str(),
            0U) != FALSE &&
            DeleteFileW(temporary.final_image().c_str()) != FALSE,
        "Synthetic final relink fixture must remove the moved object");
  const std::vector<std::byte> replacement{
      std::byte{0xa1}, std::byte{0xb1}, std::byte{0xc1}};
  write_bytes(temporary.final_image(), replacement);
  auto platform = make_platform(temporary, proof);
  auto inspected = platform->inspect_persistent_pe_exact_image_create();
  check(!inspected && read_bytes(temporary.final_image()) == replacement &&
            read_bytes(temporary.journal()) == fixture.journal_marker &&
            GetFileAttributesW(temporary.checkpoint().c_str()) !=
                INVALID_FILE_ATTRIBUTES,
        "A relinked final must remain untouched and cannot be adopted for recovery");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"restart_create_replace_and_discard",
       test_restart_create_replace_and_discard},
      {"maximum_v2_checkpoint_envelope_survives_restart_and_replace",
       test_maximum_v2_checkpoint_envelope_survives_restart_and_replace},
      {"schema_v1_slot_envelope_remains_restart_compatible",
       test_schema_v1_slot_envelope_remains_restart_compatible},
      {"owned_partial_survives_restart_and_discards_as_a_pair",
       test_owned_partial_survives_restart_and_discards_as_a_pair},
      {"checkpoint_tamper_is_bounded_and_never_discarded",
       test_checkpoint_tamper_is_bounded_and_never_discarded},
      {"replaced_partial_is_never_deleted",
       test_replaced_partial_is_never_deleted},
      {"hardlinks_are_rejected_for_checkpoint_and_partial",
       test_hardlinks_are_rejected_for_checkpoint_and_partial},
      {"reparse_data_directory_is_rejected",
       test_reparse_data_directory_is_rejected},
      {"oversize_and_unproven_backing_fail_closed",
       test_oversize_and_unproven_backing_fail_closed},
      {"stage_collision_and_locked_discard_preserve_owned_state",
       test_stage_collision_and_locked_discard_preserve_owned_state},
      {"locked_partial_rolls_back_checkpoint_discard",
       test_locked_partial_rolls_back_checkpoint_discard},
      {"replace_stage_failure_keeps_current_revision",
       test_replace_stage_failure_keeps_current_revision},
      {"v3_multi_object_slot_survives_restart_and_discards_exact_set",
       test_v3_multi_object_slot_survives_restart_and_discards_exact_set},
      {"v3_missing_or_relinked_object_is_never_adopted_or_deleted",
       test_v3_missing_or_relinked_object_is_never_adopted_or_deleted},
      {"image_commit_publishes_absent_final_then_retires_exact_set",
       test_image_commit_publishes_absent_final_then_retires_exact_set},
      {"image_commit_never_overwrites_an_existing_final",
       test_image_commit_never_overwrites_an_existing_final},
      {"image_commit_recovers_after_publish_crash_without_source_reproof",
       test_image_commit_recovers_after_publish_crash_without_source_reproof},
      {"image_commit_recovers_after_journal_retire_crash",
       test_image_commit_recovers_after_journal_retire_crash},
      {"image_post_publish_relinked_final_fails_closed",
       test_image_post_publish_relinked_final_fails_closed},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.what() << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

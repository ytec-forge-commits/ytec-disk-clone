#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/shrink_image_manifest.h"
#include "ytec/migrationengine/bundle_capture.h"
#include "ytec/migrationengine/shrink_bundle.h"
#include "ytec/migrationengine/target_layout.h"
#include "ytec/migrationengine/target_layout_io.h"
#include "ytec/migrationengine/volume_apply.h"
#include "ytec/windowsdism/dism.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::clonecore::Error mock_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::verification_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = operation,
      .message = L"モック検証失敗",
  };
}

class MockTrustVerifier final
    : public ytec::bootrepair::IExecutableTrustVerifier {
 public:
  ytec::clonecore::Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    ++call_count;
    received_path = executable_path;
    if (should_fail) {
      return ytec::clonecore::Status::failure(mock_error(L"モック署名検証"));
    }
    return ytec::clonecore::success_status();
  }

  std::wstring received_path;
  int call_count{};
  bool should_fail{};
};

class MockProcessRunner final : public ytec::bootrepair::IProcessRunner {
 public:
  ytec::clonecore::Result<ytec::bootrepair::ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    ++call_count;
    received_path = executable_path;
    received_arguments = arguments;
    received_working_directory = working_directory;
    if (on_run) {
      on_run(arguments);
    }
    return ytec::clonecore::Result<ytec::bootrepair::ProcessResult>::success(
        ytec::bootrepair::ProcessResult{
            .exit_code = exit_code,
            .standard_output = "mock stdout",
            .standard_error = "mock stderr",
        });
  }

  std::wstring received_path;
  std::wstring received_working_directory;
  std::vector<std::wstring> received_arguments;
  std::uint32_t exit_code{};
  int call_count{};
  std::function<void(const std::vector<std::wstring>&)> on_run;
};

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid value{};
    value.bytes[0] = std::byte{next_++};
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(value);
  }

 private:
  unsigned char next_{1};
};

class FixedMbrSignatureGenerator final
    : public ytec::clonecore::IMbrSignatureGenerator {
 public:
  ytec::clonecore::Result<std::uint32_t> next_signature() override {
    return ytec::clonecore::Result<std::uint32_t>::success(0xA1B2C3D4U);
  }
};

class MemoryTargetWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit MemoryTargetWriter(const std::size_t size)
      : bytes_(size, std::byte{0x7F}) {}

  std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }
  std::uint32_t logical_sector_size() const noexcept override { return 512; }
  ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > bytes_.size() || bytes.size() > bytes_.size() - offset) {
      return ytec::clonecore::Status::failure(mock_error(L"memory write"));
    }
    write_offsets.push_back(offset);
    std::copy(
        bytes.begin(),
        bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          mock_error(L"memory read"));
    }
    auto result = std::vector<std::byte>(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length));
    if (corrupt_read_back && !result.empty()) {
      result[0] ^= std::byte{1};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }
  ytec::clonecore::Status flush_target() override {
    ++flush_count;
    return ytec::clonecore::success_status();
  }

  std::vector<std::uint64_t> write_offsets;
  int flush_count{};
  bool corrupt_read_back{};

 private:
  std::vector<std::byte> bytes_;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    wchar_t root[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, root) == 0U) {
      throw TestFailure{"Could not resolve temp directory"};
    }
    wchar_t reserved[MAX_PATH]{};
    if (GetTempFileNameW(root, L"YTM", 0, reserved) == 0U ||
        !DeleteFileW(reserved)) {
      throw TestFailure{"Could not reserve temp directory"};
    }
    path_ = std::wstring(reserved) + L".dcmig";
    if (!CreateDirectoryW(path_.c_str(), nullptr)) {
      throw TestFailure{"Could not create temp .dcmig directory"};
    }
  }

  ~TemporaryDirectory() {
    for (const auto& file : files_) {
      (void)DeleteFileW(file.c_str());
    }
    (void)RemoveDirectoryW(path_.c_str());
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] std::wstring file(const std::wstring& name) {
    const std::wstring value = path_ + L"\\" + name;
    files_.push_back(value);
    return value;
  }

 private:
  std::wstring path_;
  std::vector<std::wstring> files_;
};

void write_bytes(
    const std::wstring& path,
    const std::vector<std::byte>& bytes) {
  HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw TestFailure{"Could not create temporary test file"};
  }
  DWORD written = 0;
  const bool success = bytes.empty() ||
      (WriteFile(
           handle,
           bytes.data(),
           static_cast<DWORD>(bytes.size()),
           &written,
           nullptr) &&
       written == bytes.size());
  CloseHandle(handle);
  if (!success) {
    throw TestFailure{"Could not write temporary test file"};
  }
}

ytec::imageformat::ShrinkImageManifest data_manifest(
    const std::vector<std::byte>& payload) {
  const auto hash = ytec::imageformat::sha256(payload);
  if (!hash) {
    throw TestFailure{"Could not hash synthetic payload"};
  }
  return ytec::imageformat::ShrinkImageManifest{
      .source = ytec::clonecore::StableDiskIdentity{
          .disk_number = 2,
          .model = L"Y-TEC DATA HDD",
          .size_bytes = 1000ULL * kGiB,
          .logical_sector_size = 512,
          .serial_suffix = "DATA1234",
          .device_instance_id = L"SCSI\\DISK&VEN_YTEC\\DATA",
          .is_system_disk = false,
      },
      .physical_sector_size = 4096,
      .partition_style =
          ytec::migrationcore::MigrationPartitionStyle::gpt,
      .bitlocker_fully_decrypted = true,
      .created_utc = "2026-08-03T05:00:00Z",
      .app_version = "0.2.0",
      .partitions = {
          ytec::imageformat::ShrinkImagePartition{
              .source_table_index = 1,
              .role = ytec::migrationcore::MigrationPartitionRole::data,
              .file_system =
                  ytec::migrationcore::MigrationFileSystem::ntfs,
              .source_size_bytes = 900ULL * kGiB,
              .used_bytes = 300ULL * kGiB,
              .cluster_size = 4096,
              .label = L"Data",
              .payload_file_name = "volume-001.wim",
              .payload_length_bytes = payload.size(),
              .payload_sha256 = hash.value(),
          },
      },
  };
}

void create_valid_bundle(
    TemporaryDirectory& directory,
    std::wstring& manifest_path,
    std::wstring& payload_path) {
  const std::vector<std::byte> payload{
      std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'C'}};
  payload_path = directory.file(L"volume-001.wim");
  manifest_path = directory.file(L"manifest.dcmig");
  write_bytes(payload_path, payload);
  const auto manifest = ytec::imageformat::build_shrink_image_manifest_v1(
      data_manifest(payload));
  if (!manifest) {
    throw TestFailure{"Could not build synthetic manifest"};
  }
  write_bytes(manifest_path, manifest.value());
}

void test_dism_arguments_and_trust_boundary() {
  const ytec::windowsdism::DismCaptureRequest request{
      .source_root = L"C:\\",
      .image_path = L"D:\\Backup\\volume-001.wim",
      .scratch_directory = L"D:\\Scratch",
      .image_name = L"Y-TEC volume 1",
  };
  const auto arguments =
      ytec::windowsdism::build_dism_capture_arguments(request);
  check(arguments.has_value() && arguments.value().size() == 9U,
        "Capture should use a fixed argument set");
  check(arguments.value()[0] == L"/Capture-Image" &&
            arguments.value()[4] == L"/Compress:fast" &&
            arguments.value()[7] == L"/EA",
        "Capture should use fast compression, verification, and EA support");

  MockTrustVerifier trust;
  MockProcessRunner process;
  const auto executed = ytec::windowsdism::execute_dism_capture(
      request, L"C:\\Windows\\System32", trust, process);
  check(executed.has_value() && trust.call_count == 2 &&
            process.call_count == 1 &&
            process.received_path == L"C:\\Windows\\System32\\dism.exe",
        "DISM should be Microsoft-verified before and after execution");

  trust.should_fail = true;
  trust.call_count = 0;
  process.call_count = 0;
  check(
      !ytec::windowsdism::execute_dism_capture(
           request, L"C:\\Windows\\System32", trust, process)
           .has_value() &&
          process.call_count == 0,
      "An untrusted DISM must never run");
}

void test_dism_apply_fails_closed() {
  const ytec::windowsdism::DismApplyRequest request{
      .image_path = L"D:\\Backup\\volume-001.wim",
      .target_root = L"W:\\",
      .scratch_directory = L"X:\\Scratch",
  };
  const auto arguments =
      ytec::windowsdism::build_dism_apply_arguments(request);
  check(arguments.has_value() && arguments.value().size() == 8U &&
            arguments.value()[0] == L"/Apply-Image" &&
            arguments.value()[2] == L"/Index:1",
        "Apply should use a fixed verified WIM index");
  MockTrustVerifier trust;
  MockProcessRunner process;
  process.exit_code = 5;
  check(
      !ytec::windowsdism::execute_dism_apply(
           request, L"X:\\Windows\\System32", trust, process)
           .has_value(),
      "A nonzero DISM exit code must fail closed");
  check(
      !ytec::windowsdism::build_dism_apply_arguments(
           {.image_path = L"relative.wim",
            .target_root = L"W:\\",
            .scratch_directory = L"X:\\Scratch"})
           .has_value(),
      "Relative untrusted paths must be rejected");
}

void test_bundle_verifies_and_locks_all_files() {
  TemporaryDirectory directory;
  std::wstring manifest_path;
  std::wstring payload_path;
  create_valid_bundle(directory, manifest_path, payload_path);
  auto verified =
      ytec::migrationengine::verify_shrink_bundle_read_only(manifest_path);
  check(verified.has_value() && verified.value().payloads.size() == 1U &&
            !verified.value().manifest.source.is_system_disk,
        "A canonical data-disk bundle should verify");
  HANDLE writer = CreateFileW(
      payload_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(writer == INVALID_HANDLE_VALUE,
        "Verified WIM must stay locked against replacement or writes");
  if (writer != INVALID_HANDLE_VALUE) {
    CloseHandle(writer);
  }
}

void test_bundle_rejects_tampering_and_extra_files() {
  {
    TemporaryDirectory directory;
    std::wstring manifest_path;
    std::wstring payload_path;
    create_valid_bundle(directory, manifest_path, payload_path);
    HANDLE writer = CreateFileW(
        payload_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const std::byte changed{0x7F};
    DWORD written = 0;
    const bool changed_ok = writer != INVALID_HANDLE_VALUE &&
        WriteFile(writer, &changed, 1, &written, nullptr) && written == 1U;
    if (writer != INVALID_HANDLE_VALUE) {
      CloseHandle(writer);
    }
    check(changed_ok &&
              !ytec::migrationengine::verify_shrink_bundle_read_only(
                   manifest_path)
                   .has_value(),
          "A tampered WIM must fail SHA-256 verification");
  }
  {
    TemporaryDirectory directory;
    std::wstring manifest_path;
    std::wstring payload_path;
    create_valid_bundle(directory, manifest_path, payload_path);
    write_bytes(directory.file(L"undeclared.txt"), {std::byte{1}});
    check(
        !ytec::migrationengine::verify_shrink_bundle_read_only(manifest_path)
             .has_value(),
        "Undeclared bundle files must be rejected");
  }
}

void test_bundle_capture_commits_only_verified_directory() {
  wchar_t temp_root[MAX_PATH]{};
  check(GetTempPathW(MAX_PATH, temp_root) != 0U,
        "Temp root should be available");
  wchar_t reserved[MAX_PATH]{};
  check(GetTempFileNameW(temp_root, L"YTC", 0, reserved) != 0U &&
            DeleteFileW(reserved),
        "A unique final bundle name should be reserved");
  const std::wstring final_directory = std::wstring(reserved) + L".dcmig";
  const std::vector<std::byte> placeholder{
      std::byte{'W'}, std::byte{'I'}, std::byte{'M'}};
  auto manifest = data_manifest(placeholder);
  manifest.partitions[0].payload_length_bytes = 0;
  manifest.partitions[0].payload_sha256 = {};
  ytec::migrationengine::ShrinkSourceAnalysis analysis{
      .manifest = std::move(manifest),
      .content_volumes = {
          ytec::migrationengine::AnalyzedShrinkVolume{
              .source_table_index = 1,
              .volume_guid_path = L"C:\\",
              .payload_file_name = "volume-001.wim",
          },
      },
  };
  MockTrustVerifier trust;
  MockProcessRunner process;
  process.on_run = [](const std::vector<std::wstring>& arguments) {
    const auto image = std::find_if(
        arguments.begin(), arguments.end(), [](const std::wstring& value) {
          return value.starts_with(L"/ImageFile:");
        });
    if (image == arguments.end()) {
      throw TestFailure{"Mock DISM did not receive ImageFile"};
    }
    write_bytes(image->substr(11), {
        std::byte{'W'}, std::byte{'I'}, std::byte{'M'}});
  };
  auto captured = ytec::migrationengine::capture_shrink_bundle(
      ytec::migrationengine::ShrinkBundleCaptureRequest{
          .analysis = std::move(analysis),
          .capture_sources = {
              ytec::migrationengine::ShrinkCaptureSource{
                  .source_table_index = 1,
                  .capture_root = L"C:\\",
              },
          },
          .final_bundle_directory = final_directory,
          .scratch_directory = temp_root,
      },
      L"C:\\Windows\\System32",
      trust,
      process);
  check(captured.has_value() &&
            captured.value().committed_after_complete_verification &&
            captured.value().captured_volume_count == 1U,
        "Capture should commit a completely verified .dcmig directory");
  const std::wstring payload_path = final_directory + L"\\volume-001.wim";
  const std::wstring manifest_path = final_directory + L"\\manifest.dcmig";
  check(GetFileAttributesW(payload_path.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(manifest_path.c_str()) != INVALID_FILE_ATTRIBUTES,
        "Committed bundle should contain exactly its WIM and manifest");
  (void)DeleteFileW(payload_path.c_str());
  (void)DeleteFileW(manifest_path.c_str());
  (void)RemoveDirectoryW(final_directory.c_str());
}

void test_target_layout_reconstructs_gpt_data_and_mbr_boot() {
  SequenceGuidGenerator guids;
  FixedMbrSignatureGenerator signatures;
  const std::vector<std::byte> payload{
      std::byte{'W'}, std::byte{'I'}, std::byte{'M'}};
  auto data = data_manifest(payload);
  const auto gpt = ytec::migrationengine::build_shrink_target_layout(
      data, 500ULL * kGiB, 512, guids, signatures);
  check(gpt.has_value() && gpt.value().is_gpt &&
            gpt.value().gpt.writes.size() == 5U &&
            gpt.value().gpt.target_disk.partitions.size() == 1U &&
            gpt.value().migration.source_remains_unchanged,
        "GPT data migration should create fresh target-only metadata");

  auto system = data;
  system.source.is_system_disk = true;
  system.partition_style =
      ytec::migrationcore::MigrationPartitionStyle::mbr;
  system.windows_major = 10;
  system.windows_build = 19045;
  system.windows_architecture = "AMD64";
  system.mbr_bootstrap[0] = std::byte{0xFA};
  system.partitions = {
      ytec::imageformat::ShrinkImagePartition{
          .source_table_index = 0,
          .role = ytec::migrationcore::MigrationPartitionRole::bios_system,
          .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
          .source_size_bytes = 500ULL * 1024ULL * 1024ULL,
          .used_bytes = 100ULL * 1024ULL * 1024ULL,
          .cluster_size = 4096,
          .active = true,
          .label = L"System Reserved",
      },
      ytec::imageformat::ShrinkImagePartition{
          .source_table_index = 1,
          .role = ytec::migrationcore::MigrationPartitionRole::windows,
          .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
          .source_size_bytes = 900ULL * kGiB,
          .used_bytes = 300ULL * kGiB,
          .cluster_size = 4096,
          .label = L"Windows",
      },
  };
  const auto mbr = ytec::migrationengine::build_shrink_target_layout(
      system, 500ULL * kGiB, 512, guids, signatures);
  check(mbr.has_value() && !mbr.value().is_gpt &&
            mbr.value().mbr.sector.size() == 512U &&
            mbr.value().mbr.sector[0] == std::byte{0xFA} &&
            mbr.value().mbr.target_disk.partitions[0].active,
        "MBR system migration should bind and retain source boot code");
}

void test_target_metadata_is_invalidated_verified_and_committed_last() {
  constexpr std::size_t target_size = 4U * 1024U * 1024U;
  ytec::migrationengine::ShrinkTargetLayout layout{
      .migration = ytec::migrationcore::ShrinkMigrationPlan{
          .target_style = ytec::migrationcore::MigrationPartitionStyle::mbr,
          .target_size_bytes = target_size,
      },
      .mbr = ytec::clonecore::MbrWritePlan{
          .target_disk = ytec::clonecore::MbrDisk{
              .logical_sector_size = 512,
              .sector_count = target_size / 512U,
          },
          .sector = std::vector<std::byte>(512U, std::byte{0}),
      },
      .is_gpt = false,
  };
  layout.mbr.sector[510] = std::byte{0x55};
  layout.mbr.sector[511] = std::byte{0xAA};
  MemoryTargetWriter target(target_size);
  const auto report = ytec::migrationengine::write_shrink_target_metadata(
      layout, target);
  check(
      report.has_value() && report.value().partition_table_committed &&
          report.value().every_write_read_back_verified &&
          report.value().invalidated_bytes == 2U * 1024U * 1024U &&
          target.write_offsets.size() == 3U &&
          target.write_offsets.front() == 0U &&
          target.write_offsets[1] == target_size - 1024U * 1024U &&
          target.write_offsets.back() == 0U && target.flush_count == 2,
      "Target metadata should clear both ends, read back, flush, then commit MBR last");

  MemoryTargetWriter corrupt(target_size);
  corrupt.corrupt_read_back = true;
  check(
      !ytec::migrationengine::write_shrink_target_metadata(layout, corrupt)
           .has_value(),
      "Any target read-back mismatch must fail before commit");
}

void test_format_arguments_are_fixed_and_noninteractive() {
  const auto ntfs = ytec::migrationengine::build_format_arguments(
      L"W:\\", ytec::migrationcore::MigrationFileSystem::ntfs, 4096);
  check(
      ntfs.has_value() && ntfs.value().size() == 5U &&
          ntfs.value()[0] == L"W:" && ntfs.value()[1] == L"/FS:NTFS" &&
          ntfs.value()[2] == L"/A:4096" && ntfs.value()[3] == L"/Q" &&
          ntfs.value()[4] == L"/Y",
      "FORMAT should use a fixed quick noninteractive argument set");
  check(
      !ytec::migrationengine::build_format_arguments(
           L"relative", ytec::migrationcore::MigrationFileSystem::ntfs, 4096)
           .has_value(),
      "An untrusted relative format target must be rejected");
  check(
      !ytec::migrationengine::build_format_arguments(
           L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\",
           ytec::migrationcore::MigrationFileSystem::ntfs,
           4096)
           .has_value(),
      "FORMAT must receive only a verified Mount Manager drive root");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
      {"dism_arguments_and_trust_boundary", test_dism_arguments_and_trust_boundary},
      {"dism_apply_fails_closed", test_dism_apply_fails_closed},
      {"bundle_verifies_and_locks_all_files", test_bundle_verifies_and_locks_all_files},
      {"bundle_rejects_tampering_and_extra_files", test_bundle_rejects_tampering_and_extra_files},
      {"bundle_capture_commits_only_verified_directory", test_bundle_capture_commits_only_verified_directory},
      {"target_layout_reconstructs_gpt_data_and_mbr_boot", test_target_layout_reconstructs_gpt_data_and_mbr_boot},
      {"target_metadata_is_invalidated_verified_and_committed_last", test_target_metadata_is_invalidated_verified_and_committed_last},
      {"format_arguments_are_fixed_and_noninteractive", test_format_arguments_are_fixed_and_noninteractive},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

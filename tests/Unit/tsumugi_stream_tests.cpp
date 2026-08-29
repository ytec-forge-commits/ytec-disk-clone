#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_create_resume.h"
#include "ytec/imageformat/tsumugi_stream.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::clonecore::Error test_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_READ_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

class TempDirectory final {
 public:
  TempDirectory() {
    std::array<wchar_t, MAX_PATH + 1U> root{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(root.size()), root.data());
    if (length == 0U || length >= root.size()) {
      throw TestFailure{"GetTempPathW failed"};
    }
    path_ = root.data();
    path_ += L"ytec-tsumugi-stream-";
    path_ += std::to_wstring(GetCurrentProcessId());
    path_ += L"-";
    path_ += std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(path_.c_str(), nullptr)) {
      throw TestFailure{"CreateDirectoryW failed"};
    }
  }

  ~TempDirectory() {
    const std::wstring pattern = path_ + L"\\*";
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring_view name(found.cFileName);
        if (name != L"." && name != L".." &&
            (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
          const std::wstring child = path_ + L"\\" + found.cFileName;
          static_cast<void>(SetFileAttributesW(
              child.c_str(), FILE_ATTRIBUTE_NORMAL));
          static_cast<void>(DeleteFileW(child.c_str()));
        }
      } while (FindNextFileW(search, &found));
      FindClose(search);
    }
    static_cast<void>(RemoveDirectoryW(path_.c_str()));
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] std::wstring file(const std::wstring_view name) const {
    return path_ + L"\\" + std::wstring(name);
  }

 private:
  std::wstring path_;
};

bool path_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void write_file(
    const std::wstring& path,
    const std::span<const std::byte> bytes,
    const DWORD creation = CREATE_ALWAYS) {
  const HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      creation,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw TestFailure{"CreateFileW for write failed"};
  }
  std::size_t completed = 0U;
  while (completed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - completed, 1024U * 1024U));
    DWORD written = 0U;
    if (!WriteFile(
            handle,
            bytes.data() + completed,
            amount,
            &written,
            nullptr) ||
        written != amount) {
      CloseHandle(handle);
      throw TestFailure{"WriteFile failed"};
    }
    completed += written;
  }
  CloseHandle(handle);
}

std::vector<std::byte> read_file(
    const std::wstring& path,
    const DWORD share_mode = FILE_SHARE_READ) {
  const HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      share_mode,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw TestFailure{"CreateFileW for read failed"};
  }
  LARGE_INTEGER length{};
  if (!GetFileSizeEx(handle, &length) || length.QuadPart < 0 ||
      static_cast<unsigned long long>(length.QuadPart) >
          (std::numeric_limits<std::size_t>::max)()) {
    CloseHandle(handle);
    throw TestFailure{"GetFileSizeEx failed"};
  }
  std::vector<std::byte> result(
      static_cast<std::size_t>(length.QuadPart));
  std::size_t completed = 0U;
  while (completed < result.size()) {
    const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
        result.size() - completed, 1024U * 1024U));
    DWORD read = 0U;
    if (!ReadFile(
            handle,
            result.data() + completed,
            amount,
            &read,
            nullptr) ||
        read != amount) {
      CloseHandle(handle);
      throw TestFailure{"ReadFile failed"};
    }
    completed += read;
  }
  CloseHandle(handle);
  return result;
}

DWORD try_modify_byte(
    const std::wstring& path,
    const std::uint64_t offset,
    const std::byte value) {
  const HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  DWORD written = 0U;
  DWORD error = ERROR_SUCCESS;
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    error = GetLastError();
  } else if (!WriteFile(handle, &value, 1U, &written, nullptr)) {
    error = GetLastError();
  } else if (written != 1U) {
    error = ERROR_WRITE_FAULT;
  }
  if (!CloseHandle(handle) && error == ERROR_SUCCESS) {
    error = GetLastError();
  }
  return error;
}

void modify_byte(
    const std::wstring& path,
    const std::uint64_t offset,
    const std::byte value) {
  if (try_modify_byte(path, offset, value) != ERROR_SUCCESS) {
    throw TestFailure{"byte modification failed"};
  }
}

DWORD try_open_for_write(const std::wstring& path) {
  const HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }
  if (!CloseHandle(handle)) {
    return GetLastError();
  }
  return ERROR_SUCCESS;
}

DWORD try_delete_file(const std::wstring& path) {
  if (DeleteFileW(path.c_str())) {
    return ERROR_SUCCESS;
  }
  return GetLastError();
}

DWORD try_rename_file(
    const std::wstring& source,
    const std::wstring& destination) {
  if (MoveFileExW(source.c_str(), destination.c_str(), 0U)) {
    return ERROR_SUCCESS;
  }
  return GetLastError();
}

void modify_bytes(
    const std::wstring& path,
    const std::uint64_t offset,
    const std::span<const std::byte> values) {
  const HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw TestFailure{"CreateFileW for range modification failed"};
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  DWORD written = 0U;
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
      values.size() > (std::numeric_limits<DWORD>::max)() ||
      !WriteFile(
          handle,
          values.data(),
          static_cast<DWORD>(values.size()),
          &written,
          nullptr) ||
      written != values.size()) {
    CloseHandle(handle);
    throw TestFailure{"range modification failed"};
  }
  CloseHandle(handle);
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void refresh_header_hash(const std::span<std::byte> image) {
  std::fill(image.begin() + 188U, image.begin() + 220U, std::byte{0});
  const auto digest = ytec::imageformat::sha256(
      std::span<const std::byte>(
          image.data(), ytec::imageformat::kTsumugiHeaderSize));
  check(digest.has_value(), "synthetic header hash should be calculable");
  std::copy(digest.value().begin(), digest.value().end(), image.begin() + 188U);
}

void refresh_global_hash(const std::span<std::byte> image) {
  const std::uint64_t footer_offset =
      read_little<std::uint64_t>(image, 96U);
  const auto digest = ytec::imageformat::sha256(
      std::span<const std::byte>(
          image.data(), static_cast<std::size_t>(footer_offset)));
  check(digest.has_value(), "synthetic global hash should be calculable");
  std::copy(
      digest.value().begin(),
      digest.value().end(),
      image.begin() + static_cast<std::ptrdiff_t>(footer_offset + 16U));
}

ytec::imageformat::TsumugiRescueReadEvidence rescue_read_evidence() {
  return ytec::imageformat::TsumugiRescueReadEvidence{
      .forward_attempts = 1U,
      .reverse_attempts = 1U,
      .sector_attempts = 1U,
      .zero_fill_read_back_verified = true,
      .forward_native_error = ERROR_CRC,
      .reverse_native_error = ERROR_SECTOR_NOT_FOUND,
      .sector_native_error = ERROR_READ_FAULT,
  };
}

class MemorySource final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemorySource(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_count;
    if (fail_read || offset > bytes_.size() ||
        length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成Source読取り", L"注入した読取り失敗です"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  }

  mutable std::size_t read_count{};
  bool fail_read{};

 private:
  std::vector<std::byte> bytes_;
};

std::vector<std::byte> source_bytes() {
  std::vector<std::byte> bytes(8192U);
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>((index * 17U) & 0xFFU);
  }
  return bytes;
}

std::array<std::byte, 16> image_id() {
  std::array<std::byte, 16> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = static_cast<std::byte>(0x20U + index);
  }
  return result;
}

ytec::imageformat::TsumugiEncryptionSettings encryption_settings() {
  ytec::imageformat::TsumugiEncryptionSettings encryption{};
  encryption.password = "Correct horse 42!";
  for (std::size_t index = 0U;
       index < encryption.argon2.salt.size();
       ++index) {
    encryption.argon2.salt[index] =
        static_cast<std::byte>(0x40U + index);
  }
  for (std::size_t index = 0U;
       index < encryption.base_nonce.size();
       ++index) {
    encryption.base_nonce[index] =
        static_cast<std::byte>(0x60U + index);
  }
  return encryption;
}

ytec::imageformat::TsumugiStreamBuildRequest stream_request(
    const std::wstring& path,
    const MemorySource& source,
    const bool encrypted = false) {
  ytec::imageformat::TsumugiStreamBuildRequest request{};
  request.final_path = path;
  request.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  request.logical_sector_size = 512U;
  request.physical_sector_size = 4096U;
  request.compression =
      ytec::imageformat::ImageCompression::zstandard;
  request.verification_block_bytes = 1024U;
  request.image_id = image_id();
  request.manifest = {
      std::byte{'M'}, std::byte{'A'}, std::byte{'N'}, std::byte{'1'}};
  request.chunks = {
      ytec::imageformat::TsumugiStreamBuildChunk{
          .logical_offset = 0U,
          .logical_length = 4096U,
          .source_offset = 0U,
          .flags = ytec::imageformat::TsumugiChunkFlags::none,
          .source = &source,
      },
      ytec::imageformat::TsumugiStreamBuildChunk{
          .logical_offset = 16ULL * 1024ULL * 1024ULL,
          .logical_length = 4096U,
          .source_offset = 4096U,
          .flags = ytec::imageformat::TsumugiChunkFlags::none,
          .source = &source,
      },
      ytec::imageformat::TsumugiStreamBuildChunk{
          .logical_offset = 32ULL * 1024ULL * 1024ULL,
          .logical_length = 8192U,
          .source_offset = 0U,
          .flags = ytec::imageformat::TsumugiChunkFlags::zero_filled,
          .source = nullptr,
      },
  };
  if (encrypted) {
    request.encryption = encryption_settings();
  }
  return request;
}

ytec::imageformat::TsumugiCreateResumeBindingV1 resume_binding() {
  ytec::imageformat::TsumugiCreateResumeBindingV1 binding{};
  for (std::size_t index = 0U; index < binding.operation_id.size(); ++index) {
    binding.operation_id[index] = static_cast<std::byte>(0x10U + index);
  }
  const auto fill_digest = [](auto& digest, const std::uint8_t seed) {
    for (std::size_t index = 0U; index < digest.size(); ++index) {
      digest[index] = static_cast<std::byte>(seed + index);
    }
  };
  fill_digest(binding.plan_hash, 0x20U);
  fill_digest(binding.source_identity_hash, 0x40U);
  fill_digest(binding.source_state_hash, 0x60U);
  fill_digest(binding.destination_storage_identity_hash, 0x80U);
  fill_digest(binding.output_identity_hash, 0xA0U);
  return binding;
}

class ResumeCheckpointHarness final {
 public:
  [[nodiscard]] ytec::imageformat::TsumugiCreateResumeCheckpointHooksV1
  hooks() {
    return {
        .create_before_first_mutation =
            [this](
                const auto& paths,
                const auto& binding,
                const auto& progress) {
              try {
                const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE;
                saw_two_empty_create_objects =
                    read_file(paths.image_partial_path, share).empty() &&
                    read_file(paths.journal_path, share).empty();
              } catch (...) {
                return ytec::clonecore::Status::failure(test_error(
                    L"合成resume checkpoint create",
                    L"create callbackからowned objectを確認できません"));
              }
              paths_ = paths;
              binding_ = binding;
              current = progress;
              ++create_calls;
              if (fail_create_after_record) {
                return ytec::clonecore::Status::failure(test_error(
                    L"合成resume checkpoint create",
                    L"永続化後の読戻し失敗を注入しました"));
              }
              return ytec::clonecore::success_status();
            },
        .prove_existing_before_resume =
            [this](
                const auto& paths,
                const auto& binding,
                const auto& progress) {
              ++prove_calls;
              if (reject_proof || !current.has_value() ||
                  paths.image_partial_path != paths_.image_partial_path ||
                  paths.journal_path != paths_.journal_path ||
                  binding != binding_ || progress != *current) {
                return ytec::clonecore::Status::failure(test_error(
                    L"合成resume checkpoint proof",
                    L"source state、出力objectまたはcheckpoint束縛が一致しません"));
              }
              return ytec::clonecore::success_status();
            },
        .replace_after_verified_prefix =
            [this](const auto& before, const auto& after) {
              ++replace_calls;
              if (!current.has_value() || before != *current) {
                return ytec::clonecore::Status::failure(test_error(
                    L"合成resume checkpoint replace",
                    L"before revisionが現在値と一致しません"));
              }
              if (fail_replace_once ||
                  (fail_replace_at_verified_chunk.has_value() &&
                   after.verified_chunk_count ==
                       *fail_replace_at_verified_chunk)) {
                fail_replace_once = false;
                fail_replace_at_verified_chunk.reset();
                return ytec::clonecore::Status::failure(test_error(
                    L"合成resume checkpoint replace",
                    L"crash前のreplace失敗を注入しました"));
              }
              current = after;
              return ytec::clonecore::success_status();
            },
    };
  }

  std::optional<ytec::imageformat::TsumugiCreateResumeProgressV1> current;
  bool saw_two_empty_create_objects{};
  bool fail_create_after_record{};
  bool reject_proof{};
  bool fail_replace_once{};
  std::optional<std::uint64_t> fail_replace_at_verified_chunk;
  std::size_t create_calls{};
  std::size_t prove_calls{};
  std::size_t replace_calls{};

 private:
  ytec::imageformat::TsumugiCreateResumeOwnedPathsV1 paths_;
  ytec::imageformat::TsumugiCreateResumeBindingV1 binding_;
};

ytec::imageformat::TsumugiCreateResumeRequestV1 resume_request(
    ytec::imageformat::TsumugiStreamBuildRequest stream,
    ResumeCheckpointHarness& checkpoint,
    const std::optional<
        ytec::imageformat::TsumugiCreateResumeProgressV1> expected =
        std::nullopt) {
  return ytec::imageformat::TsumugiCreateResumeRequestV1{
      .stream = std::move(stream),
      .binding = resume_binding(),
      .expected_progress = expected,
      .checkpoint = checkpoint.hooks(),
  };
}

ytec::imageformat::Sha256Digest resume_test_domain_hash(
    const std::string_view domain,
    const std::span<const std::byte> bytes) {
  std::vector<std::byte> canonical;
  canonical.reserve(domain.size() + bytes.size());
  for (const char value : domain) {
    canonical.push_back(static_cast<std::byte>(value));
  }
  canonical.insert(canonical.end(), bytes.begin(), bytes.end());
  const auto digest = ytec::imageformat::sha256(canonical);
  check(digest.has_value(), "resume test domain hash should succeed");
  return digest.value();
}

void refresh_resume_journal_header_hashes(
    const std::span<std::byte> journal) {
  check(journal.size() >= 512U, "resume journal header fixture is too short");
  auto header = journal.first(512U);
  std::fill(header.begin() + 400U, header.begin() + 464U, std::byte{0});
  const auto root = resume_test_domain_hash(
      "YTEC-TSUMUGI-CREATE-RESUME-ROOT-V1", header);
  std::copy(root.begin(), root.end(), header.begin() + 400U);
  std::fill(header.begin() + 432U, header.begin() + 464U, std::byte{0});
  const auto header_hash = resume_test_domain_hash(
      "YTEC-TSUMUGI-CREATE-RESUME-HEADER-V1", header);
  std::copy(header_hash.begin(), header_hash.end(), header.begin() + 432U);
}

void refresh_resume_journal_frame_chain(
    const std::span<std::byte> frame) {
  check(frame.size() == 240U, "resume journal frame fixture size mismatch");
  std::fill(frame.begin() + 200U, frame.begin() + 232U, std::byte{0});
  const auto chain = resume_test_domain_hash(
      "YTEC-TSUMUGI-CREATE-RESUME-FRAME-V1", frame);
  std::copy(chain.begin(), chain.end(), frame.begin() + 200U);
}

ytec::imageformat::TsumugiBuildRequest memory_request(
    const std::vector<std::byte>& bytes,
    const bool encrypted = false) {
  ytec::imageformat::TsumugiBuildRequest request{};
  request.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  request.logical_sector_size = 512U;
  request.physical_sector_size = 4096U;
  request.compression =
      ytec::imageformat::ImageCompression::zstandard;
  request.image_id = image_id();
  request.manifest = {
      std::byte{'M'}, std::byte{'A'}, std::byte{'N'}, std::byte{'1'}};
  request.chunks = {
      ytec::imageformat::TsumugiBuildChunk{
          .logical_offset = 0U,
          .logical_length = 4096U,
          .flags = ytec::imageformat::TsumugiChunkFlags::none,
          .data = std::vector<std::byte>(bytes.begin(), bytes.begin() + 4096),
      },
      ytec::imageformat::TsumugiBuildChunk{
          .logical_offset = 16ULL * 1024ULL * 1024ULL,
          .logical_length = 4096U,
          .flags = ytec::imageformat::TsumugiChunkFlags::none,
          .data = std::vector<std::byte>(bytes.begin() + 4096, bytes.end()),
      },
      ytec::imageformat::TsumugiBuildChunk{
          .logical_offset = 32ULL * 1024ULL * 1024ULL,
          .logical_length = 8192U,
          .flags = ytec::imageformat::TsumugiChunkFlags::zero_filled,
          .data = {},
      },
  };
  if (encrypted) {
    request.encryption = encryption_settings();
  }
  return request;
}

void test_stream_matches_canonical_in_memory_format() {
  TempDirectory temp;
  const auto bytes = source_bytes();
  MemorySource source(bytes);
  const std::wstring path = temp.file(L"normal.tsumugi");
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> boundaries;
  std::vector<ytec::clonecore::DiskOperationProgress> progress;
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      stream_request(path, source),
      ytec::clonecore::DiskOperationCallbacks{
          .progress = [&](const auto& value) { progress.push_back(value); },
          .safe_boundary = [&](const auto& boundary) {
            boundaries.push_back(boundary);
            return ytec::clonecore::DiskOperationControlDecision::
                continue_operation;
          },
      });
  check(result.has_value(), "valid streaming creation should pass");
  check(result.value().committed &&
            result.value().all_chunks_read_back_verified &&
            result.value().all_chunks_authenticated_and_hashed &&
            result.value().global_hash_read_back_verified &&
            result.value().final_metadata_read_back_verified &&
            result.value().final_complete_scan_performed &&
            result.value().verification_mode ==
                ytec::imageformat::TsumugiCreateVerificationMode::complete,
        "completion must require both read-back gates");
  check(path_exists(path) && !path_exists(path + L".partial"),
        "only the completed name should remain");
  check(source.read_count == 2U,
        "each non-zero source chunk should be read exactly once");
  check(
      std::count_if(
          boundaries.begin(), boundaries.end(), [](const auto& boundary) {
            return boundary.stage ==
                ytec::clonecore::DiskOperationStage::copying_data;
          }) == 3,
      "every logical image chunk must expose one verified creation boundary");
  check(
      std::count_if(
          boundaries.begin(), boundaries.end(), [](const auto& boundary) {
            return boundary.stage ==
                       ytec::clonecore::DiskOperationStage::verifying_final &&
                boundary.kind == ytec::clonecore::
                    DiskOperationSafeBoundaryKind::verified_chunk;
          }) == 3,
      "complete creation verification must rescan every plaintext chunk");
  check(
      !progress.empty() && !progress.back().pause_allowed &&
          progress.back().stage ==
              ytec::clonecore::DiskOperationStage::completed,
      "image final-name commit must end in a non-pausable completed state");

  const auto canonical =
      ytec::imageformat::build_tsumugi_v1(memory_request(bytes));
  check(canonical.has_value(), "in-memory reference image should build");
  check(read_file(path) == canonical.value(),
        "stream and in-memory writers must emit identical v1 bytes");
}

void test_fast_creation_keeps_write_time_and_metadata_verification() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"fast-encrypted.tsumugi");
  auto request = stream_request(path, source, true);
  request.verification_mode =
      ytec::imageformat::TsumugiCreateVerificationMode::fast;
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> boundaries;
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      request,
      ytec::clonecore::DiskOperationCallbacks{
          .safe_boundary = [&](const auto& boundary) {
            boundaries.push_back(boundary);
            return ytec::clonecore::DiskOperationControlDecision::
                continue_operation;
          },
      });
  check(result.has_value(), "fast encrypted creation should pass");
  check(
      result.value().committed &&
          result.value().verification_mode ==
              ytec::imageformat::TsumugiCreateVerificationMode::fast &&
          result.value().all_chunks_read_back_verified &&
          result.value().all_chunks_authenticated_and_hashed &&
          result.value().global_hash_read_back_verified &&
          result.value().final_metadata_read_back_verified &&
          !result.value().final_complete_scan_performed,
      "fast mode must retain immediate authentication/hash/metadata gates and report no extra full scan");
  check(source.read_count == 2U,
        "fast mode must read each source payload chunk exactly once");
  check(
      std::none_of(
          boundaries.begin(), boundaries.end(), [](const auto& boundary) {
            return boundary.stage ==
                       ytec::clonecore::DiskOperationStage::verifying_final &&
                boundary.kind == ytec::clonecore::
                    DiskOperationSafeBoundaryKind::verified_chunk;
          }),
      "fast mode must omit only the additional plaintext-chunk scan");

  const auto verified = ytec::imageformat::verify_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .password = request.encryption->password,
          .verification_block_bytes = 1024U,
      });
  check(verified.has_value() && verified.value().all_chunks_verified &&
            verified.value().global_hash_verified &&
            verified.value().metadata_authenticated,
        "an image created in fast mode must still pass an independent complete verification");
}

void test_unknown_creation_verification_mode_is_rejected_before_source_read() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"unknown-mode.tsumugi");
  auto request = stream_request(path, source);
  request.verification_mode = static_cast<
      ytec::imageformat::TsumugiCreateVerificationMode>(0xffU);
  const auto result =
      ytec::imageformat::write_verified_tsumugi_file_v1(request);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::invalid_argument,
        "an unknown creation verification mode must fail closed");
  check(source.read_count == 0U && !path_exists(path) &&
            !path_exists(path + L".partial"),
        "unknown mode rejection must precede source reads and output creation");
}

void test_stream_rescue_evidence_round_trip_and_tamper_rejection() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"rescue.tsumugi");
  auto request = stream_request(path, source);
  request.payload_kind = ytec::imageformat::TsumugiPayloadKind::rescue_disk;
  request.chunks[2].flags =
      ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled;
  const auto evidence = rescue_read_evidence();
  request.chunks[2].rescue_read_evidence = evidence;

  const auto written =
      ytec::imageformat::write_verified_tsumugi_file_v1(request);
  check(written.has_value(),
        "a streamed rescue image with retry evidence should build");
  const auto verified = ytec::imageformat::verify_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .verification_block_bytes = 1024U,
      });
  check(verified.has_value() && verified.value().records.size() == 3U &&
            verified.value().records[2].rescue_read_evidence == evidence &&
            (verified.value().header.required_features &
             static_cast<std::uint32_t>(
                 ytec::imageformat::TsumugiRequiredFeature::
                     rescue_read_evidence)) != 0U,
        "stream verification must expose the authenticated retry evidence");

  auto tampered = read_file(path);
  const auto image = std::span<std::byte>(tampered);
  const std::uint64_t metadata_offset =
      read_little<std::uint64_t>(image, 80U);
  const std::uint64_t manifest_length = read_little<std::uint64_t>(
      image, static_cast<std::size_t>(metadata_offset) + 16U);
  const std::size_t third_record_offset =
      static_cast<std::size_t>(metadata_offset) +
      ytec::imageformat::kTsumugiMetadataHeaderSize +
      static_cast<std::size_t>(manifest_length) +
      (2U * ytec::imageformat::kTsumugiChunkRecordSize);

  const std::wstring legacy_path = temp.file(L"legacy-rescue.tsumugi");
  auto legacy = tampered;
  const auto legacy_image = std::span<std::byte>(legacy);
  write_little<std::uint32_t>(
      legacy_image,
      20U,
      read_little<std::uint32_t>(legacy_image, 20U) & ~4U);
  std::fill(
      legacy_image.begin() +
          static_cast<std::ptrdiff_t>(third_record_offset + 96U),
      legacy_image.begin() +
          static_cast<std::ptrdiff_t>(third_record_offset + 112U),
      std::byte{0});
  refresh_header_hash(legacy_image);
  refresh_global_hash(legacy_image);
  write_file(legacy_path, legacy);
  const auto legacy_verified = ytec::imageformat::verify_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = legacy_path,
          .verification_block_bytes = 1024U,
      });
  check(legacy_verified.has_value() &&
            !legacy_verified.value().records[2]
                 .rescue_read_evidence.has_value(),
        "the stream reader must retain pre-extension rescue compatibility");

  image[third_record_offset + 96U] = std::byte{0};
  refresh_global_hash(image);
  write_file(path, tampered);

  check(!ytec::imageformat::verify_tsumugi_file_v1(
             ytec::imageformat::TsumugiStreamVerifyRequest{
                 .image_path = path,
                 .verification_block_bytes = 1024U,
             })
             .has_value(),
        "invalid retry evidence must fail after an outer-hash refresh");
}

void test_verify_then_deliver_and_hold_input_immutable() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"restore.tsumugi");
  check(ytec::imageformat::write_verified_tsumugi_file_v1(
            stream_request(path, source)).has_value(),
        "fixture image should build");

  std::size_t callbacks = 0U;
  std::uint64_t logical_bytes = 0U;
  bool write_open_was_blocked = false;
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> boundaries;
  std::vector<ytec::clonecore::DiskOperationProgress> progress;
  const auto restored = ytec::imageformat::read_verified_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .verification_block_bytes = 1024U,
      },
      [&](const ytec::imageformat::TsumugiChunkRecord& record,
          const std::span<const std::byte> plaintext) {
        ++callbacks;
        logical_bytes += record.logical_length;
        if (callbacks == 1U) {
          const HANDLE writer = CreateFileW(
              path.c_str(),
              GENERIC_WRITE,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
              nullptr,
              OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL,
              nullptr);
          write_open_was_blocked = writer == INVALID_HANDLE_VALUE;
          if (writer != INVALID_HANDLE_VALUE) {
            CloseHandle(writer);
          }
        }
        const bool zero =
            (static_cast<std::uint32_t>(record.flags) & 1U) != 0U;
        check(zero == plaintext.empty(),
              "only zero records should use an empty plaintext span");
        return ytec::clonecore::success_status();
      },
      ytec::clonecore::DiskOperationCallbacks{
          .progress = [&](const auto& value) { progress.push_back(value); },
          .safe_boundary = [&](const auto& boundary) {
            boundaries.push_back(boundary);
            return ytec::clonecore::DiskOperationControlDecision::
                continue_operation;
          },
      });
  check(restored.has_value(), "verified two-pass delivery should pass");
  check(restored.value().callbacks_started_after_complete_verification &&
            callbacks == 3U && logical_bytes == 16'384U,
        "all callbacks should start only after complete first-pass verification");
  check(write_open_was_blocked,
        "the one input handle must deny concurrent write and delete access");
  check(
      std::count_if(
          boundaries.begin(), boundaries.end(), [](const auto& boundary) {
            return boundary.stage ==
                ytec::clonecore::DiskOperationStage::copying_data &&
                boundary.kind ==
                ytec::clonecore::DiskOperationSafeBoundaryKind::
                    verified_chunk;
          }) == 3,
      "restore must pause only after each delivered verified chunk");
  check(
      !progress.empty() &&
          progress.back().stage ==
              ytec::clonecore::DiskOperationStage::flushing_data &&
          !progress.back().pause_allowed,
      "the caller's physical commit interval must be visibly non-pausable");
}

void test_corruption_calls_no_restore_callback() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"corrupt.tsumugi");
  const auto built = ytec::imageformat::write_verified_tsumugi_file_v1(
      stream_request(path, source));
  check(built.has_value(), "fixture image should build");
  modify_byte(path, built.value().image_length - 65U, std::byte{0xA5});

  std::size_t callbacks = 0U;
  const auto restored = ytec::imageformat::read_verified_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .verification_block_bytes = 1024U,
      },
      [&](const ytec::imageformat::TsumugiChunkRecord&,
          const std::span<const std::byte>) {
        ++callbacks;
        return ytec::clonecore::success_status();
      });
  check(!restored.has_value(), "tampered payload must be rejected");
  check(callbacks == 0U,
        "no target-write callback may run before whole-image verification");
}

void test_same_handle_inspection_gate_precedes_restore_callbacks() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"plan-gate.tsumugi");
  check(ytec::imageformat::write_verified_tsumugi_file_v1(
            stream_request(path, source)).has_value(),
        "fixture image should build");

  std::size_t gate_calls = 0U;
  std::size_t restore_callbacks = 0U;
  const auto restored = ytec::imageformat::read_verified_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .verification_block_bytes = 1024U,
      },
      [&](const ytec::imageformat::TsumugiChunkRecord&,
          const std::span<const std::byte>) {
        ++restore_callbacks;
        return ytec::clonecore::success_status();
      },
      {},
      [&](const ytec::imageformat::TsumugiStreamInspection& inspection) {
        ++gate_calls;
        check(inspection.all_chunks_verified &&
                  inspection.global_hash_verified,
              "gate should see the completed first verification pass");
        return ytec::clonecore::Status::failure({
            .code = ytec::clonecore::ErrorCode::identity_mismatch,
            .native_code = ERROR_FILE_INVALID,
            .operation = L"モック復元計画照合",
            .message = L"検査した画像と計画が一致しません",
        });
      });
  check(!restored.has_value() && gate_calls == 1U &&
            restore_callbacks == 0U,
        "a rejected same-handle plan gate must authorize no target callback");
}

void test_encrypted_stream_roundtrip_and_wrong_password_rejection() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"encrypted.tsumugi");
  const auto built = ytec::imageformat::write_verified_tsumugi_file_v1(
      stream_request(path, source, true));
  check(built.has_value(), "encrypted streaming creation should pass");

  const auto verified = ytec::imageformat::verify_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .password = "Correct horse 42!",
          .verification_block_bytes = 1024U,
      });
  check(verified.has_value() && verified.value().metadata_authenticated,
        "correct password must authenticate metadata and chunks");
  const auto wrong = ytec::imageformat::verify_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .password = "Wrong password!",
          .verification_block_bytes = 1024U,
      });
  check(!wrong.has_value(), "wrong password must fail closed");

  const auto canonical = ytec::imageformat::build_tsumugi_v1(
      memory_request(source_bytes(), true));
  check(canonical.has_value() && read_file(path) == canonical.value(),
        "encrypted stream bytes must match the canonical v1 writer");
}

void test_cancellation_never_promotes_partial_or_replaces_old_final() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"cancel.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  std::uint64_t safe_boundary_count = 0U;
  auto request = stream_request(path, source);
  request.replace_existing = true;
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      request,
      ytec::clonecore::DiskOperationCallbacks{
          .safe_boundary = [&](const auto& boundary) {
            if (boundary.stage ==
                ytec::clonecore::DiskOperationStage::copying_data) {
              ++safe_boundary_count;
              return ytec::clonecore::DiskOperationControlDecision::
                  cancel_operation;
            }
            return ytec::clonecore::DiskOperationControlDecision::
                continue_operation;
          },
      });
  check(!result.has_value() &&
            result.error().code == ytec::clonecore::ErrorCode::cancelled,
        "cancellation should use the dedicated error code");
  check(read_file(path) == std::vector<std::byte>(old.begin(), old.end()),
        "cancellation must leave the old completed image byte-exact");
  check(
      safe_boundary_count == 1U,
      "image cancellation must stop at the first verified chunk boundary");
  check(!path_exists(path + L".partial") &&
            !path_exists(path + L".replace-backup"),
        "cancelled output must not promote or retain an owned partial");
}

void test_existing_unknown_partial_is_untouched() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"owned.tsumugi");
  const std::array<std::byte, 3> sentinel{
      std::byte{'N'}, std::byte{'O'}, std::byte{'!'}};
  write_file(path + L".partial", sentinel);
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      stream_request(path, source));
  check(!result.has_value(), "pre-existing unknown partial must block creation");
  check(read_file(path + L".partial") ==
            std::vector<std::byte>(sentinel.begin(), sentinel.end()),
        "unknown partial must never be overwritten or deleted");
  check(!path_exists(path), "blocked creation must not create a final image");
}

void test_existing_recovery_file_blocks_even_when_final_is_absent() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"recovery.tsumugi");
  const std::array<std::byte, 4> sentinel{
      std::byte{'S'}, std::byte{'A'}, std::byte{'F'}, std::byte{'E'}};
  write_file(path + L".replace-backup", sentinel);
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      stream_request(path, source));
  check(!result.has_value(),
        "a crash-recovery file must block creation even without a final name");
  check(read_file(path + L".replace-backup") ==
            std::vector<std::byte>(sentinel.begin(), sentinel.end()),
        "a pre-existing recovery file must remain byte-exact");
  check(!path_exists(path) && !path_exists(path + L".partial"),
        "blocked recovery must not create any new image path");
}

void test_verified_replacement_uses_recoverable_transaction() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"replace.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  auto request = stream_request(path, source);
  request.replace_existing = true;
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      request);
  check(result.has_value() && result.value().replaced_existing,
        "verified image should replace an explicitly allowed old final");
  check(result.value().retained_recovery_path.empty(),
        "normal replacement should remove the recovery copy after commit");
  check(!path_exists(path + L".replace-backup") &&
            !path_exists(path + L".partial"),
        "successful transaction should leave no staging names");
  check(ytec::imageformat::verify_tsumugi_file_v1(
            ytec::imageformat::TsumugiStreamVerifyRequest{
                .image_path = path,
                .verification_block_bytes = 1024U,
            }).has_value(),
        "replacement final must remain a completely verified image");
}

void test_replacement_requires_explicit_request() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"no-replace.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      stream_request(path, source));
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "replacement must require an explicit request flag");
  check(read_file(path) == std::vector<std::byte>(old.begin(), old.end()) &&
            !path_exists(path + L".partial"),
        "implicit replacement refusal must leave the old final untouched");
}

void test_unknown_required_feature_and_overflow_call_no_callback() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"untrusted.tsumugi");
  check(ytec::imageformat::write_verified_tsumugi_file_v1(
            stream_request(path, source)).has_value(),
        "fixture image should build");
  // required_features begins at byte 20. An unknown mandatory bit must be
  // rejected before any restoration callback, even though this also makes the
  // header hash stale.
  modify_byte(path, 23U, std::byte{0x80});
  std::size_t callbacks = 0U;
  const auto result = ytec::imageformat::read_verified_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .verification_block_bytes = 1024U,
      },
      [&](const ytec::imageformat::TsumugiChunkRecord&,
          const std::span<const std::byte>) {
        ++callbacks;
        return ytec::clonecore::success_status();
      });
  check(!result.has_value() && callbacks == 0U,
        "unknown mandatory features must fail closed before callback release");

  const std::wstring overflow_path = temp.file(L"overflow.tsumugi");
  check(ytec::imageformat::write_verified_tsumugi_file_v1(
            stream_request(overflow_path, source)).has_value(),
        "overflow fixture image should build");
  auto overflow_image = read_file(overflow_path);
  check(overflow_image.size() >= ytec::imageformat::kTsumugiHeaderSize,
        "overflow fixture should contain a complete header");
  auto header = std::span<std::byte>(
      overflow_image.data(), ytec::imageformat::kTsumugiHeaderSize);
  // data.length is the second uint64 at the data section (header byte 72).
  write_little(
      header, 72U, (std::numeric_limits<std::uint64_t>::max)());
  std::fill(header.begin() + 188U, header.begin() + 220U, std::byte{0});
  const auto digest = ytec::imageformat::sha256(header);
  check(digest.has_value(), "tampered header hash should be calculable");
  std::copy(
      digest.value().begin(),
      digest.value().end(),
      header.begin() + 188U);
  modify_bytes(overflow_path, 0U, header);

  callbacks = 0U;
  const auto overflow = ytec::imageformat::read_verified_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = overflow_path,
          .verification_block_bytes = 1024U,
      },
      [&](const ytec::imageformat::TsumugiChunkRecord&,
          const std::span<const std::byte>) {
        ++callbacks;
        return ytec::clonecore::success_status();
      });
  check(!overflow.has_value() && callbacks == 0U,
        "overflowed sections must fail before global reads or callbacks");
}

void test_final_path_write_is_locked_during_creation() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"locked-final.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  auto request = stream_request(path, source);
  request.replace_existing = true;
  bool attempted = false;
  DWORD mutation_error = ERROR_SUCCESS;
  const auto result = ytec::imageformat::write_verified_tsumugi_file_v1(
      request,
      ytec::clonecore::DiskOperationCallbacks{
          .progress =
              [&](const ytec::clonecore::DiskOperationProgress& progress) {
                if (!attempted && progress.stage ==
                                      ytec::clonecore::DiskOperationStage::
                                          verifying_final) {
                  mutation_error =
                      try_modify_byte(path, 0U, std::byte{'X'});
                  attempted = true;
                }
              },
      });
  check(attempted && mutation_error == ERROR_SHARING_VIOLATION &&
            result.has_value() && result.value().replaced_existing &&
            result.value().retained_recovery_path.empty(),
        "an existing final must reject writers until verified replacement commits");
  const auto verified = ytec::imageformat::verify_tsumugi_file_v1(
      ytec::imageformat::TsumugiStreamVerifyRequest{
          .image_path = path,
          .verification_block_bytes = 1024U,
      });
  check(verified.has_value() && verified.value().all_chunks_verified &&
            verified.value().global_hash_verified,
        "the lock-protected replacement must publish a verified image");
  check(!path_exists(path + L".partial") &&
            !path_exists(path + L".replace-backup"),
        "lock-protected replacement must leave no transaction files");
  check(try_open_for_write(path) == ERROR_SUCCESS,
        "a successful commit must release the final-file guard");
}

void test_preexisting_writer_blocks_before_partial_creation() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"preexisting-writer.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  const HANDLE writer = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  check(writer != INVALID_HANDLE_VALUE,
        "pre-existing writer fixture must open");
  auto request = stream_request(path, source);
  request.replace_existing = true;
  auto staged = ytec::imageformat::prepare_verified_tsumugi_file_v1(request);
  const BOOL closed = CloseHandle(writer);

  check(closed && !staged.has_value() &&
            staged.error().code ==
                ytec::clonecore::ErrorCode::access_denied &&
            staged.error().native_code == ERROR_SHARING_VIOLATION,
        "replacement must fail closed while an existing writer is open");
  check(!path_exists(path + L".partial") &&
            !path_exists(path + L".replace-backup") &&
            read_file(path) ==
                std::vector<std::byte>(old.begin(), old.end()),
        "writer conflict must be detected before partial creation");
}

void test_staged_file_is_invisible_until_single_commit() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"staged.tsumugi");
  auto staged = ytec::imageformat::prepare_verified_tsumugi_file_v1(
      stream_request(path, source));
  check(staged.has_value() && staged.value().pending(),
        "a completely verified staged file should remain pending");
  check(!staged.value().report().committed && !path_exists(path) &&
            path_exists(path + L".partial"),
        "preparation must keep the completed name invisible");

  const auto committed = staged.value().commit_verified();
  check(committed.has_value() && committed.value().committed &&
            !staged.value().pending(),
        "the explicit commit should expose one verified final image");
  check(path_exists(path) && !path_exists(path + L".partial"),
        "commit should atomically consume the owned partial");
  check(!staged.value().commit_verified().has_value(),
        "a staged transaction must reject a second commit");
}

void test_staged_abort_preserves_existing_final() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"staged-abort.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  auto request = stream_request(path, source);
  request.replace_existing = true;
  auto staged = ytec::imageformat::prepare_verified_tsumugi_file_v1(request);
  check(staged.has_value() && path_exists(path + L".partial"),
        "replacement preparation should retain an owned partial");
  check(read_file(path, FILE_SHARE_READ | FILE_SHARE_DELETE) ==
            std::vector<std::byte>(old.begin(), old.end()),
        "the existing final must remain byte-exact before commit");
  check(try_open_for_write(path) == ERROR_SHARING_VIOLATION,
        "a pending replacement must keep the existing final write-locked");

  const auto aborted = staged.value().abort_incomplete();
  check(aborted.has_value() && !staged.value().pending(),
        "explicit abort should finish the staged transaction");
  check(!path_exists(path + L".partial") &&
            read_file(path) ==
                std::vector<std::byte>(old.begin(), old.end()),
        "abort must delete only its own partial and preserve the old final");
  check(try_open_for_write(path) == ERROR_SUCCESS,
        "abort must release the existing-final guard immediately");
  check(!staged.value().commit_verified().has_value(),
        "an aborted staged transaction must not be reusable");
}

void test_staged_commit_retains_final_path_lock() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"staged-lock.tsumugi");
  const std::wstring renamed = temp.file(L"staged-lock-moved.tsumugi");
  const std::array<std::byte, 4> old{
      std::byte{'O'}, std::byte{'L'}, std::byte{'D'}, std::byte{'!'}};
  write_file(path, old);
  auto request = stream_request(path, source);
  request.replace_existing = true;
  auto staged = ytec::imageformat::prepare_verified_tsumugi_file_v1(request);
  check(staged.has_value(), "locked staged fixture should prepare");
  const DWORD write_error =
      try_modify_byte(path, 0U, std::byte{'X'});
  const DWORD delete_error = try_delete_file(path);
  const DWORD rename_error = try_rename_file(path, renamed);
  check(write_error == ERROR_SHARING_VIOLATION &&
            delete_error == ERROR_SHARING_VIOLATION &&
            rename_error == ERROR_SHARING_VIOLATION &&
            path_exists(path) && !path_exists(renamed) &&
            read_file(path, FILE_SHARE_READ | FILE_SHARE_DELETE) ==
                std::vector<std::byte>(old.begin(), old.end()),
        "pending commit must block writer, delete, and rename access");

  const auto committed = staged.value().commit_verified();
  check(committed.has_value() && committed.value().replaced_existing &&
            committed.value().retained_recovery_path.empty() &&
            !path_exists(path + L".partial") &&
            !path_exists(path + L".replace-backup"),
        "delayed commit must replace only the continuously guarded final");
  check(try_open_for_write(path) == ERROR_SUCCESS,
        "delayed commit must release the final-file guard");
}

void test_resume_exact_reuses_verified_prefix_and_finishes_commit_ready() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"resume-exact.tsumugi");
  ResumeCheckpointHarness checkpoint;
  auto first_request = resume_request(
      stream_request(path, source), checkpoint);
  const auto interrupted =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(
          first_request,
          ytec::clonecore::DiskOperationCallbacks{
              .safe_boundary = [](const auto& boundary) {
                if (boundary.kind == ytec::clonecore::
                        DiskOperationSafeBoundaryKind::verified_chunk &&
                    boundary.stage ==
                        ytec::clonecore::DiskOperationStage::copying_data &&
                    boundary.completed_units == 1U) {
                  return ytec::clonecore::DiskOperationControlDecision::
                      cancel_operation;
                }
                return ytec::clonecore::DiskOperationControlDecision::
                    continue_operation;
              },
          });
  check(!interrupted.has_value() &&
            interrupted.error().code ==
                ytec::clonecore::ErrorCode::cancelled &&
            checkpoint.current.has_value() &&
            checkpoint.current->phase == ytec::imageformat::
                TsumugiCreateResumePhaseV1::prepared &&
            checkpoint.current->verified_chunk_count == 1U &&
            checkpoint.saw_two_empty_create_objects &&
            checkpoint.create_calls == 1U && source.read_count == 1U,
        "new resume create must durably bind two empty objects before mutation and stop at one verified chunk");
  const std::wstring partial = path + L".partial";
  const std::wstring journal = path + L".resume-journal.partial";
  check(path_exists(partial) && path_exists(journal) && !path_exists(path),
        "cancelled persistent create must retain only owned private files");

  const auto journal_before = read_file(journal);
  const auto inspected_before =
      ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
          journal_before);
  check(inspected_before.has_value() &&
            inspected_before.value().chunks.size() == 1U,
        "interrupted journal must be independently parseable");
  const auto partial_before = read_file(partial);
  const std::size_t payload_begin = static_cast<std::size_t>(
      inspected_before.value().header.payload_offset);
  const std::size_t durable_end = static_cast<std::size_t>(
      checkpoint.current->primary_output_length);
  check(payload_begin <= durable_end && durable_end <= partial_before.size(),
        "durable payload prefix must be within the owned partial");
  const std::vector<std::byte> durable_payload(
      partial_before.begin() + static_cast<std::ptrdiff_t>(payload_begin),
      partial_before.begin() + static_cast<std::ptrdiff_t>(durable_end));

  auto continued_request = resume_request(
      stream_request(path, source), checkpoint, checkpoint.current);
  const auto completed =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(
          continued_request);
  check(completed.has_value() && completed.value().resumed &&
            completed.value().commit_ready &&
            completed.value().complete_partial_verified &&
            completed.value().reused_verified_chunk_count == 1U &&
            completed.value().appended_chunk_count == 2U &&
            completed.value().progress.phase == ytec::imageformat::
                TsumugiCreateResumePhaseV1::commit_ready &&
            checkpoint.prove_calls == 1U && source.read_count == 2U,
        "resume must re-read/authenticate the durable prefix and read only source suffix chunks");
  const auto partial_after = read_file(partial);
  check(partial_after.size() >= durable_end &&
            std::equal(
                durable_payload.begin(),
                durable_payload.end(),
                partial_after.begin() +
                    static_cast<std::ptrdiff_t>(payload_begin)),
        "resume must not rewrite the previously verified payload prefix");
  const auto independently_verified = ytec::imageformat::inspect_tsumugi_v1(
      partial_after);
  check(independently_verified.has_value() &&
            independently_verified.value().all_chunks_verified &&
            independently_verified.value().global_hash_verified &&
            !path_exists(path),
        "commit-ready partial must be a complete v1 image while final name remains untouched");
}

void test_resume_password_and_source_proof_fail_without_mutation() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"resume-encrypted.tsumugi");
  ResumeCheckpointHarness checkpoint;
  auto first_request = resume_request(
      stream_request(path, source, true), checkpoint);
  const auto interrupted =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(
          first_request,
          ytec::clonecore::DiskOperationCallbacks{
              .safe_boundary = [](const auto& boundary) {
                return boundary.kind == ytec::clonecore::
                               DiskOperationSafeBoundaryKind::verified_chunk &&
                        boundary.stage == ytec::clonecore::
                                DiskOperationStage::copying_data &&
                        boundary.completed_units == 1U
                    ? ytec::clonecore::DiskOperationControlDecision::
                          cancel_operation
                    : ytec::clonecore::DiskOperationControlDecision::
                          continue_operation;
              },
          });
  check(!interrupted.has_value() && checkpoint.current.has_value(),
        "encrypted resume fixture should stop after one durable chunk");
  const std::wstring partial = path + L".partial";
  const std::wstring journal = path + L".resume-journal.partial";
  const auto partial_before = read_file(partial);
  const auto journal_before = read_file(journal);
  const auto checkpoint_before = *checkpoint.current;

  auto wrong_stream = stream_request(path, source, true);
  wrong_stream.encryption->password = "Definitely wrong 99!";
  auto wrong_request = resume_request(
      std::move(wrong_stream), checkpoint, checkpoint.current);
  const auto wrong = ytec::imageformat::prepare_resumable_tsumugi_file_v1(
      wrong_request);
  check(!wrong.has_value() && read_file(partial) == partial_before &&
            read_file(journal) == journal_before &&
            *checkpoint.current == checkpoint_before,
        "wrong re-entered password must reject the authenticated prefix without mutation");

  checkpoint.reject_proof = true;
  auto changed_source_request = resume_request(
      stream_request(path, source, true), checkpoint, checkpoint.current);
  const auto changed_source =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(
          changed_source_request);
  check(!changed_source.has_value() && read_file(partial) == partial_before &&
            read_file(journal) == journal_before &&
            *checkpoint.current == checkpoint_before,
        "changed source-state proof must fail before reading/truncating owned objects");

  checkpoint.reject_proof = false;
  auto correct_request = resume_request(
      stream_request(path, source, true), checkpoint, checkpoint.current);
  const auto completed =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(correct_request);
  check(completed.has_value() && completed.value().commit_ready &&
            completed.value().stream.final_complete_scan_performed,
        "correct password re-entry must permit full commit-ready verification");
}

void test_resume_recovers_only_uncommitted_suffix_after_replace_failure() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"resume-replace-failure.tsumugi");
  ResumeCheckpointHarness checkpoint;
  checkpoint.fail_replace_at_verified_chunk = 1U;
  auto first_request = resume_request(
      stream_request(path, source), checkpoint);
  const auto failed = ytec::imageformat::prepare_resumable_tsumugi_file_v1(
      first_request);
  check(!failed.has_value() && checkpoint.current.has_value() &&
            checkpoint.current->verified_chunk_count == 0U &&
            path_exists(path + L".partial") &&
            path_exists(path + L".resume-journal.partial") &&
            read_file(path + L".partial").size() >
                checkpoint.current->primary_output_length &&
            read_file(path + L".resume-journal.partial").size() >
                checkpoint.current->journal_length,
        "checkpoint replace failure must retain an uncommitted suffix without advancing durable progress");

  auto continued_request = resume_request(
      stream_request(path, source), checkpoint, checkpoint.current);
  const auto completed =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(
          continued_request);
  check(completed.has_value() && completed.value().commit_ready &&
            completed.value().reused_verified_chunk_count == 0U &&
            completed.value().appended_chunk_count == 3U &&
            source.read_count == 3U,
        "resume must verify the durable prefix, truncate only the uncommitted suffix, and rebuild it");
}

void test_resume_ambiguous_initial_checkpoint_failure_retains_objects() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"resume-ambiguous-create.tsumugi");
  ResumeCheckpointHarness checkpoint;
  checkpoint.fail_create_after_record = true;
  auto request = resume_request(stream_request(path, source), checkpoint);

  const auto failed = ytec::imageformat::prepare_resumable_tsumugi_file_v1(
      request);
  check(!failed.has_value() && checkpoint.current.has_value() &&
            checkpoint.current->phase == ytec::imageformat::
                TsumugiCreateResumePhaseV1::preparing &&
            checkpoint.saw_two_empty_create_objects &&
            path_exists(path + L".partial") &&
            path_exists(path + L".resume-journal.partial") &&
            read_file(path + L".partial").empty() &&
            read_file(path + L".resume-journal.partial").empty() &&
            source.read_count == 0U,
        "a persistence-ambiguous initial checkpoint failure must retain both exact empty objects without source reads");
}

void test_resume_journal_parser_rejects_bounded_untrusted_shapes() {
  TempDirectory temp;
  MemorySource source(source_bytes());
  const std::wstring path = temp.file(L"resume-parser.tsumugi");
  ResumeCheckpointHarness checkpoint;
  auto request = resume_request(stream_request(path, source), checkpoint);
  const auto interrupted =
      ytec::imageformat::prepare_resumable_tsumugi_file_v1(
          request,
          ytec::clonecore::DiskOperationCallbacks{
              .safe_boundary = [](const auto& boundary) {
                return boundary.kind == ytec::clonecore::
                               DiskOperationSafeBoundaryKind::verified_chunk &&
                        boundary.stage == ytec::clonecore::
                                DiskOperationStage::copying_data &&
                        boundary.completed_units == 2U
                    ? ytec::clonecore::DiskOperationControlDecision::
                          cancel_operation
                    : ytec::clonecore::DiskOperationControlDecision::
                          continue_operation;
              },
          });
  check(!interrupted.has_value() && checkpoint.current.has_value() &&
            checkpoint.current->verified_chunk_count == 2U,
        "parser test fixture must contain two chained records");
  const auto valid = read_file(path + L".resume-journal.partial");
  check(ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(valid)
            .has_value(),
        "resume journal fixture must be valid before negative mutations");

  auto truncated = valid;
  truncated.pop_back();
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             truncated)
             .has_value(),
        "truncated frame must fail closed");

  const std::vector<std::byte> oversize(
      ytec::imageformat::kTsumugiCreateResumeJournalMaximumBytes + 1U,
      std::byte{0});
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             oversize)
             .has_value(),
        "journal above fixed maximum size must reject before parsing");

  auto unknown_version = valid;
  write_little<std::uint16_t>(unknown_version, 10U, 1U);
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             unknown_version)
             .has_value(),
        "unknown journal minor version must fail closed");

  auto count_overflow = valid;
  write_little<std::uint64_t>(
      count_overflow,
      48U,
      ytec::imageformat::kTsumugiCreateResumeJournalMaximumRecords + 1U);
  refresh_resume_journal_header_hashes(count_overflow);
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             count_overflow)
             .has_value(),
        "record count above fixed allocation bound must fail closed");

  auto duplicate_index = valid;
  write_little<std::uint64_t>(duplicate_index, 512U + 240U + 16U, 0U);
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             duplicate_index)
             .has_value(),
        "duplicate record key/index must fail closed");

  auto chain_mismatch = valid;
  chain_mismatch[512U + 168U] ^= std::byte{1U};
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             chain_mismatch)
             .has_value(),
        "previous-chain mismatch must fail closed");

  auto length_overflow = valid;
  write_little<std::uint64_t>(
      length_overflow,
      512U + 48U,
      (std::numeric_limits<std::uint64_t>::max)());
  refresh_resume_journal_frame_chain(
      std::span<std::byte>(length_overflow).subspan(512U, 240U));
  check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
             length_overflow)
             .has_value(),
        "stored-length overflow must fail closed before range use");

  std::uint64_t state = 0x59544543524A4E4CULL;
  for (std::size_t sample = 0U; sample < 128U; ++sample) {
    const std::size_t length = 512U + (sample * 37U) % 721U;
    std::vector<std::byte> random(length);
    for (auto& value : random) {
      state ^= state << 13U;
      state ^= state >> 7U;
      state ^= state << 17U;
      value = static_cast<std::byte>(state & 0xFFU);
    }
    check(!ytec::imageformat::inspect_tsumugi_create_resume_journal_v1(
               random)
               .has_value(),
          "deterministic random journal bytes must fail closed");
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"stream_matches_canonical_in_memory_format",
       test_stream_matches_canonical_in_memory_format},
      {"fast_creation_keeps_write_time_and_metadata_verification",
       test_fast_creation_keeps_write_time_and_metadata_verification},
      {"unknown_creation_verification_mode_is_rejected_before_source_read",
       test_unknown_creation_verification_mode_is_rejected_before_source_read},
      {"stream_rescue_evidence_round_trip_and_tamper_rejection",
       test_stream_rescue_evidence_round_trip_and_tamper_rejection},
      {"verify_then_deliver_and_hold_input_immutable",
       test_verify_then_deliver_and_hold_input_immutable},
      {"corruption_calls_no_restore_callback",
       test_corruption_calls_no_restore_callback},
      {"same_handle_inspection_gate_precedes_restore_callbacks",
       test_same_handle_inspection_gate_precedes_restore_callbacks},
      {"encrypted_stream_roundtrip_and_wrong_password_rejection",
       test_encrypted_stream_roundtrip_and_wrong_password_rejection},
      {"cancellation_never_promotes_partial_or_replaces_old_final",
       test_cancellation_never_promotes_partial_or_replaces_old_final},
      {"existing_unknown_partial_is_untouched",
       test_existing_unknown_partial_is_untouched},
      {"existing_recovery_file_blocks_even_when_final_is_absent",
       test_existing_recovery_file_blocks_even_when_final_is_absent},
      {"verified_replacement_uses_recoverable_transaction",
       test_verified_replacement_uses_recoverable_transaction},
      {"replacement_requires_explicit_request",
       test_replacement_requires_explicit_request},
      {"unknown_required_feature_and_overflow_call_no_callback",
       test_unknown_required_feature_and_overflow_call_no_callback},
      {"final_path_write_is_locked_during_creation",
       test_final_path_write_is_locked_during_creation},
      {"preexisting_writer_blocks_before_partial_creation",
       test_preexisting_writer_blocks_before_partial_creation},
      {"staged_file_is_invisible_until_single_commit",
       test_staged_file_is_invisible_until_single_commit},
      {"staged_abort_preserves_existing_final",
       test_staged_abort_preserves_existing_final},
      {"staged_commit_retains_final_path_lock",
       test_staged_commit_retains_final_path_lock},
      {"resume_exact_reuses_verified_prefix_and_finishes_commit_ready",
       test_resume_exact_reuses_verified_prefix_and_finishes_commit_ready},
      {"resume_password_and_source_proof_fail_without_mutation",
       test_resume_password_and_source_proof_fail_without_mutation},
      {"resume_recovers_only_uncommitted_suffix_after_replace_failure",
       test_resume_recovers_only_uncommitted_suffix_after_replace_failure},
      {"resume_ambiguous_initial_checkpoint_failure_retains_objects",
       test_resume_ambiguous_initial_checkpoint_failure_retains_objects},
      {"resume_journal_parser_rejects_bounded_untrusted_shapes",
       test_resume_journal_parser_rejects_bounded_untrusted_shapes},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

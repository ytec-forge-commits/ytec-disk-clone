#include "ytec/migrationengine/windows_file_system_recreate.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_restore_layout_io.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winioctl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

constexpr std::size_t kDirectoryQueryBytes = 64U * 1024U;
constexpr std::size_t kHashReadBytes = 1024U * 1024U;
constexpr std::uint64_t kFatEpochFileTime =
    119'600'064'000'000'000ULL;

constexpr ULONG kFileDirectoryFile = 0x00000001UL;
constexpr ULONG kFileNonDirectoryFile = 0x00000040UL;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020UL;
constexpr ULONG kFileOpenForBackupIntent = 0x00004000UL;
constexpr ULONG kFileOpenReparsePoint = 0x00200000UL;
constexpr ULONG kFileOpen = 0x00000001UL;

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE,
    ACCESS_MASK,
    POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK,
    PLARGE_INTEGER,
    ULONG,
    ULONG,
    ULONG,
    ULONG,
    PVOID,
    ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(NTSTATUS);

clonecore::Error recreate_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(recreate_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(recreate_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool is_zero_digest(
    const migrationcore::FileSystemRecreateSha256& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte item) {
    return item == std::byte{};
  });
}

bool is_supported_recreate_file_system(
    const migrationcore::MigrationFileSystem file_system) noexcept {
  return file_system == migrationcore::MigrationFileSystem::fat32 ||
      file_system == migrationcore::MigrationFileSystem::exfat;
}

bool same_text_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size() ||
      left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  if (left.empty()) {
    return true;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

struct OrdinalCaseInsensitiveLess final {
  bool operator()(
      const std::wstring& left,
      const std::wstring& right) const noexcept {
    if (left.empty() || right.empty() ||
        left.size() >
            static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() >
            static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
      return left < right;
    }
    const int compared = CompareStringOrdinal(
        left.data(),
        static_cast<int>(left.size()),
        right.data(),
        static_cast<int>(right.size()),
        TRUE);
    return compared == CSTR_LESS_THAN ||
        (compared == 0 && left < right);
  }
};

bool same_stable_device(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  if (!left.serial_suffix.empty() && !right.serial_suffix.empty() &&
      left.serial_suffix == right.serial_suffix &&
      left.model == right.model) {
    return true;
  }
  return !left.device_instance_id.empty() &&
      left.device_instance_id == right.device_instance_id;
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(std::byte{value});
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(std::byte{
        static_cast<std::uint8_t>((value >> shift) & 0xFFU)});
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(std::byte{
        static_cast<std::uint8_t>((value >> shift) & 0xFFU)});
  }
}

bool append_narrow(
    std::vector<std::byte>& bytes,
    const std::string_view value) {
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return false;
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const char item : value) {
    bytes.push_back(std::byte{static_cast<std::uint8_t>(item)});
  }
  return true;
}

bool append_wide(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return false;
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t item : value) {
    const auto unit = static_cast<std::uint16_t>(item);
    bytes.push_back(std::byte{static_cast<std::uint8_t>(unit & 0xFFU)});
    bytes.push_back(std::byte{static_cast<std::uint8_t>(unit >> 8U)});
  }
  return true;
}

void append_digest(
    std::vector<std::byte>& bytes,
    const migrationcore::FileSystemRecreateSha256& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

bool append_stable_identity(
    std::vector<std::byte>& bytes,
    const clonecore::StableDiskIdentity& identity) {
  if (!append_wide(bytes, identity.model)) {
    return false;
  }
  append_u64(bytes, identity.size_bytes);
  append_u32(bytes, identity.logical_sector_size);
  if (!append_narrow(bytes, identity.serial_suffix) ||
      !append_wide(bytes, identity.device_instance_id)) {
    return false;
  }
  append_u8(bytes, identity.is_system_disk ? 1U : 0U);
  return true;
}

clonecore::Result<migrationcore::FileSystemRecreateSha256>
hash_target_binding(
    const clonecore::StableDiskIdentity& disk,
    const std::uint32_t partition_number,
    const std::uint64_t partition_offset,
    const std::uint64_t partition_length) {
  std::vector<std::byte> bytes;
  bytes.reserve(512U);
  if (!append_narrow(bytes, "Y-TEC-FS-RECREATE-TARGET-BINDING-V1") ||
      !append_stable_identity(bytes, disk)) {
    return failure<migrationcore::FileSystemRecreateSha256>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成コピー先binding Hash",
        L"安定識別文字列がHash形式上限を超えています");
  }
  append_u32(bytes, partition_number);
  append_u64(bytes, partition_offset);
  append_u64(bytes, partition_length);
  return imageformat::sha256(bytes);
}

clonecore::Result<migrationcore::FileSystemRecreateSha256>
hash_execution_plan(
    const migrationcore::FileSystemRecreatePlan& plan,
    const clonecore::StableDiskIdentity& source,
    const migrationcore::FileSystemRecreateSha256& target_binding) {
  std::vector<std::byte> bytes;
  bytes.reserve(512U);
  if (!append_narrow(bytes, "Y-TEC-FS-RECREATE-EXECUTION-PLAN-V1")) {
    return failure<migrationcore::FileSystemRecreateSha256>(
        clonecore::ErrorCode::internal_error,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成実行計画Hash",
        L"実行計画Hash prefixを表現できません");
  }
  append_digest(bytes, plan.plan_sha256());
  append_digest(bytes, plan.source_epoch_sha256());
  if (!append_stable_identity(bytes, source)) {
    return failure<migrationcore::FileSystemRecreateSha256>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成実行計画Hash",
        L"コピー元安定識別がHash形式上限を超えています");
  }
  append_digest(bytes, target_binding);
  return imageformat::sha256(bytes);
}

DWORD native_status_error(const NTSTATUS status) noexcept {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return ERROR_GEN_FAILURE;
  }
  const auto converter = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
      GetProcAddress(ntdll, "RtlNtStatusToDosError"));
  return converter == nullptr
      ? ERROR_GEN_FAILURE
      : static_cast<DWORD>(converter(status));
}

clonecore::Result<NtCreateFileFunction> nt_create_file_function() {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return clonecore::Result<NtCreateFileFunction>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::internal_error,
            L"再作成source NtCreateFile取得",
            GetLastError()));
  }
  const auto function = reinterpret_cast<NtCreateFileFunction>(
      GetProcAddress(ntdll, "NtCreateFile"));
  if (function == nullptr) {
    return clonecore::Result<NtCreateFileFunction>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::unsupported_platform,
            L"再作成source NtCreateFile取得",
            GetLastError()));
  }
  return clonecore::Result<NtCreateFileFunction>::success(function);
}

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
      (value >= L'a' && value <= L'f') ||
      (value >= L'A' && value <= L'F');
}

bool is_guid_body(const std::wstring_view value) noexcept {
  if (value.size() != 36U) {
    return false;
  }
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const bool hyphen = index == 8U || index == 13U || index == 18U ||
        index == 23U;
    if ((hyphen && value[index] != L'-') ||
        (!hyphen && !is_hex(value[index]))) {
      return false;
    }
  }
  return true;
}

std::optional<WindowsFileSystemRecreateSourceRootKind>
classify_source_root(std::wstring_view path) noexcept {
  if (path.ends_with(L'\\')) {
    path.remove_suffix(1U);
  }
  if (path.empty() || path.ends_with(L'\\') ||
      path.find(L'/') != path.npos) {
    return std::nullopt;
  }

  constexpr std::wstring_view volume_prefix = L"\\\\?\\Volume{";
  if (path.starts_with(volume_prefix) && path.ends_with(L'}') &&
      is_guid_body(path.substr(
          volume_prefix.size(),
          path.size() - volume_prefix.size() - 1U))) {
    return WindowsFileSystemRecreateSourceRootKind::canonical_volume_guid;
  }

  constexpr std::wstring_view snapshot_prefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (!path.starts_with(snapshot_prefix) ||
      path.size() <= snapshot_prefix.size()) {
    return std::nullopt;
  }
  const auto suffix = path.substr(snapshot_prefix.size());
  if (!std::all_of(suffix.begin(), suffix.end(), [](const wchar_t value) {
        return value >= L'0' && value <= L'9';
      })) {
    return std::nullopt;
  }
  return WindowsFileSystemRecreateSourceRootKind::vss_snapshot_device;
}

std::wstring normalized_root_path(std::wstring value) {
  if (!value.ends_with(L'\\')) {
    value.push_back(L'\\');
  }
  return value;
}

std::wstring root_open_path(std::wstring value) {
  while (value.ends_with(L'\\')) {
    value.pop_back();
  }
  return value;
}

clonecore::Result<clonecore::UniqueHandle> open_source_root_read_only(
    const std::wstring& source_root) {
  const std::wstring path = root_open_path(source_root);
  clonecore::UniqueHandle root(CreateFileW(
      path.c_str(),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!root) {
    const DWORD native_code = GetLastError();
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            native_code == ERROR_ACCESS_DENIED
                ? clonecore::ErrorCode::access_denied
                : clonecore::ErrorCode::io_failed,
            L"再作成source root read-only open",
            native_code));
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      std::move(root));
}

clonecore::Result<clonecore::UniqueHandle> open_relative_object_read_only(
    const HANDLE parent,
    const std::wstring_view component,
    const bool directory) {
  if (parent == nullptr || parent == INVALID_HANDLE_VALUE ||
      component.empty() ||
      component.size() >
          migrationcore::kMaximumFileSystemRecreateComponentUtf16Units ||
      component.size() >
          (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t) ||
      component.find_first_of(L"\\/") != component.npos) {
    return failure<clonecore::UniqueHandle>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"再作成source handle-relative open",
        L"親handleまたは相対componentが固定上限外です");
  }

  auto native = nt_create_file_function();
  if (!native) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(native.error());
  }
  UNICODE_STRING relative{};
  relative.Buffer = const_cast<PWSTR>(component.data());
  relative.Length = static_cast<USHORT>(component.size() * sizeof(wchar_t));
  relative.MaximumLength = relative.Length;
  OBJECT_ATTRIBUTES attributes{};
  InitializeObjectAttributes(
      &attributes,
      &relative,
      OBJ_CASE_INSENSITIVE,
      parent,
      nullptr);
  IO_STATUS_BLOCK io{};
  HANDLE opened = INVALID_HANDLE_VALUE;
  const ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE |
      (directory ? FILE_LIST_DIRECTORY : FILE_READ_DATA);
  const ULONG options = kFileOpenReparsePoint |
      kFileOpenForBackupIntent | kFileSynchronousIoNonAlert |
      (directory ? kFileDirectoryFile : kFileNonDirectoryFile);
  const NTSTATUS status = native.value()(
      &opened,
      access,
      &attributes,
      &io,
      nullptr,
      directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      kFileOpen,
      options,
      nullptr,
      0U);
  if (status < 0) {
    const DWORD native_code = native_status_error(status);
    if (opened != nullptr && opened != INVALID_HANDLE_VALUE) {
      CloseHandle(opened);
    }
    return failure<clonecore::UniqueHandle>(
        native_code == ERROR_ACCESS_DENIED
            ? clonecore::ErrorCode::access_denied
            : clonecore::ErrorCode::io_failed,
        native_code,
        L"再作成source handle-relative open",
        L"RootDirectory相対・reparse非追跡でsource objectを開けませんでした");
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      clonecore::UniqueHandle(opened));
}

class Sha256Accumulator final {
 public:
  Sha256Accumulator() = default;
  Sha256Accumulator(const Sha256Accumulator&) = delete;
  Sha256Accumulator& operator=(const Sha256Accumulator&) = delete;
  ~Sha256Accumulator() {
    if (hash_ != nullptr) {
      BCryptDestroyHash(hash_);
    }
    if (algorithm_ != nullptr) {
      BCryptCloseAlgorithmProvider(algorithm_, 0U);
    }
  }

  [[nodiscard]] clonecore::Status initialize() {
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    ULONG object_size{};
    ULONG returned{};
    ULONG digest_size{};
    if (status >= 0) {
      status = BCryptGetProperty(
          algorithm_,
          BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_size),
          sizeof(object_size),
          &returned,
          0U);
    }
    if (status >= 0) {
      status = BCryptGetProperty(
          algorithm_,
          BCRYPT_HASH_LENGTH,
          reinterpret_cast<PUCHAR>(&digest_size),
          sizeof(digest_size),
          &returned,
          0U);
    }
    if (status < 0 || object_size == 0U ||
        digest_size != migrationcore::FileSystemRecreateSha256{}.size()) {
      return status_failure(
          clonecore::ErrorCode::internal_error,
          native_status_error(status),
          L"再作成source SHA-256初期化",
          L"Windows CNG SHA-256の固定出力を初期化できませんでした");
    }
    object_.resize(object_size);
    status = BCryptCreateHash(
        algorithm_,
        &hash_,
        object_.data(),
        object_size,
        nullptr,
        0U,
        0U);
    if (status < 0 || hash_ == nullptr) {
      return status_failure(
          clonecore::ErrorCode::internal_error,
          native_status_error(status),
          L"再作成source SHA-256生成",
          L"Windows CNG SHA-256状態を生成できませんでした");
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status update(
      const std::span<const std::byte> bytes) {
    if (hash_ == nullptr || finished_ ||
        bytes.size() > (std::numeric_limits<ULONG>::max)()) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"再作成source SHA-256更新",
          L"SHA-256状態または入力長が不正です");
    }
    const NTSTATUS status = BCryptHashData(
        hash_,
        reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),
        0U);
    if (status < 0) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          native_status_error(status),
          L"再作成source SHA-256更新",
          L"source streamをSHA-256へ追加できませんでした");
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<
      migrationcore::FileSystemRecreateSha256>
  finish() {
    if (hash_ == nullptr || finished_) {
      return failure<migrationcore::FileSystemRecreateSha256>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"再作成source SHA-256確定",
          L"SHA-256状態が未初期化または確定済みです");
    }
    migrationcore::FileSystemRecreateSha256 digest{};
    const NTSTATUS status = BCryptFinishHash(
        hash_,
        reinterpret_cast<PUCHAR>(digest.data()),
        static_cast<ULONG>(digest.size()),
        0U);
    if (status < 0) {
      return failure<migrationcore::FileSystemRecreateSha256>(
          clonecore::ErrorCode::verification_failed,
          native_status_error(status),
          L"再作成source SHA-256確定",
          L"source stream SHA-256を確定できませんでした");
    }
    finished_ = true;
    return clonecore::Result<
        migrationcore::FileSystemRecreateSha256>::success(digest);
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_{};
  BCRYPT_HASH_HANDLE hash_{};
  std::vector<UCHAR> object_;
  bool finished_{};
};

struct StreamSummary final {
  std::uint32_t named_stream_count{};
  std::uint32_t unnamed_data_stream_count{};
};

clonecore::Result<StreamSummary> query_stream_summary(
    const HANDLE handle,
    const bool directory,
    const std::size_t maximum_query_bytes) {
  std::size_t buffer_size = 4096U;
  std::vector<std::byte> buffer;
  for (;;) {
    if (buffer_size > maximum_query_bytes ||
        buffer_size > (std::numeric_limits<DWORD>::max)()) {
      return failure<StreamSummary>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_BUFFER_OVERFLOW,
          L"再作成source FileStreamInfo",
          L"stream情報が固定query上限を超えました");
    }
    buffer.assign(buffer_size, std::byte{});
    if (GetFileInformationByHandleEx(
            handle,
            FileStreamInfo,
            buffer.data(),
            static_cast<DWORD>(buffer.size())) != FALSE) {
      break;
    }
    const DWORD native_code = GetLastError();
    if ((native_code == ERROR_HANDLE_EOF ||
         native_code == ERROR_NO_MORE_FILES) &&
        directory) {
      return clonecore::Result<StreamSummary>::success(StreamSummary{});
    }
    // FAT32/exFAT cannot represent named data streams.  Some Windows builds
    // report that fact by rejecting FileStreamInfo instead of returning the
    // single unnamed stream.  Only the documented unsupported-query family is
    // accepted; access, I/O and malformed-buffer failures remain fatal.
    if (native_code == ERROR_INVALID_FUNCTION ||
        native_code == ERROR_NOT_SUPPORTED ||
        native_code == ERROR_INVALID_PARAMETER) {
      return clonecore::Result<StreamSummary>::success(StreamSummary{
          .named_stream_count = 0U,
          .unnamed_data_stream_count = directory ? 0U : 1U,
      });
    }
    if (native_code != ERROR_MORE_DATA &&
        native_code != ERROR_INSUFFICIENT_BUFFER) {
      return clonecore::Result<StreamSummary>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"再作成source FileStreamInfo",
              native_code));
    }
    if (buffer_size > maximum_query_bytes / 2U) {
      return failure<StreamSummary>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_BUFFER_OVERFLOW,
          L"再作成source FileStreamInfo",
          L"stream情報が固定query上限を超えました");
    }
    buffer_size *= 2U;
  }

  StreamSummary summary;
  std::size_t offset = 0U;
  constexpr std::size_t header_bytes = offsetof(FILE_STREAM_INFO, StreamName);
  for (;;) {
    if (offset > buffer.size() - header_bytes ||
        offset % alignof(FILE_STREAM_INFO) != 0U) {
      return failure<StreamSummary>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"再作成source FileStreamInfo境界",
          L"stream entry headerがbuffer境界外です");
    }
    const auto* info = reinterpret_cast<const FILE_STREAM_INFO*>(
        buffer.data() + offset);
    if ((info->StreamNameLength % sizeof(wchar_t)) != 0U ||
        info->StreamNameLength >
            buffer.size() - offset - header_bytes) {
      return failure<StreamSummary>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"再作成source FileStreamInfo名",
          L"stream名の長さまたはUTF-16境界が不正です");
    }
    const std::wstring_view name(
        info->StreamName,
        info->StreamNameLength / sizeof(wchar_t));
    if (same_text_case_insensitive(name, L"::$DATA")) {
      if (summary.unnamed_data_stream_count ==
          (std::numeric_limits<std::uint32_t>::max)()) {
        return failure<StreamSummary>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"再作成source unnamed stream件数",
            L"unnamed stream件数が表現上限を超えました");
      }
      ++summary.unnamed_data_stream_count;
    } else {
      if (summary.named_stream_count ==
          (std::numeric_limits<std::uint32_t>::max)()) {
        return failure<StreamSummary>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"再作成source named stream件数",
            L"named stream件数が表現上限を超えました");
      }
      ++summary.named_stream_count;
    }
    if (info->NextEntryOffset == 0U) {
      break;
    }
    if (info->NextEntryOffset < header_bytes ||
        info->NextEntryOffset > buffer.size() - offset) {
      return failure<StreamSummary>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"再作成source FileStreamInfo連鎖",
          L"stream entry offsetがbuffer境界外です");
    }
    offset += info->NextEntryOffset;
  }
  return clonecore::Result<StreamSummary>::success(summary);
}

using FileId128 = std::array<std::byte, 16U>;

struct ExactHandleMetadata final {
  FileId128 file_id{};
  std::uint64_t volume_serial_number{};
  std::uint64_t size_bytes{};
  std::uint64_t allocation_bytes{};
  std::uint64_t creation_time{};
  std::uint64_t last_access_time{};
  std::uint64_t last_write_time{};
  std::uint64_t change_time{};
  std::uint32_t file_attributes{};
  std::uint32_t hard_link_count{};
  std::uint32_t reparse_tag{};
  std::uint32_t named_stream_count{};
  std::uint32_t unnamed_stream_count{};
  bool directory{};
};

bool same_exact_metadata(
    const ExactHandleMetadata& left,
    const ExactHandleMetadata& right) noexcept {
  return left.file_id == right.file_id &&
      left.volume_serial_number == right.volume_serial_number &&
      left.size_bytes == right.size_bytes &&
      left.allocation_bytes == right.allocation_bytes &&
      left.creation_time == right.creation_time &&
      left.last_access_time == right.last_access_time &&
      left.last_write_time == right.last_write_time &&
      left.change_time == right.change_time &&
      left.file_attributes == right.file_attributes &&
      left.hard_link_count == right.hard_link_count &&
      left.reparse_tag == right.reparse_tag &&
      left.named_stream_count == right.named_stream_count &&
      left.unnamed_stream_count == right.unnamed_stream_count &&
      left.directory == right.directory;
}

clonecore::Result<ExactHandleMetadata> query_exact_handle_metadata(
    const HANDLE handle,
    const bool expected_directory,
    const migrationcore::MigrationFileSystem file_system,
    const WindowsFileSystemRecreateSourceLimits& limits) {
  FILE_ID_INFO identifier{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (GetFileInformationByHandleEx(
          handle, FileIdInfo, &identifier, sizeof(identifier)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
      GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) == FALSE ||
      GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE) {
    return clonecore::Result<ExactHandleMetadata>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"再作成source opened-handle metadata",
            GetLastError()));
  }

  const auto streams = query_stream_summary(
      handle, expected_directory, limits.maximum_stream_query_bytes);
  if (!streams) {
    return clonecore::Result<ExactHandleMetadata>::failure(streams.error());
  }

  const bool directory = standard.Directory != FALSE;
  const std::uint32_t unsupported_attributes =
      FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SPARSE_FILE |
      FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED |
      FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_DEVICE;
  const std::uint64_t format_maximum =
      file_system == migrationcore::MigrationFileSystem::fat32
      ? migrationcore::kFat32MaximumRecreatedFileBytes
      : migrationcore::kExfatMaximumRecreatedFileBytes;
  if (standard.DeletePending != FALSE || directory != expected_directory ||
      standard.EndOfFile.QuadPart < 0 ||
      standard.AllocationSize.QuadPart < 0 ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) >
          limits.maximum_file_bytes ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) >
          format_maximum ||
      standard.NumberOfLinks != 1U ||
      (attributes.FileAttributes & unsupported_attributes) != 0U ||
      attributes.ReparseTag != 0U ||
      streams.value().named_stream_count != 0U ||
      (!directory && streams.value().unnamed_data_stream_count != 1U) ||
      basic.CreationTime.QuadPart < 0 ||
      basic.LastAccessTime.QuadPart < 0 ||
      basic.LastWriteTime.QuadPart < 0 ||
      basic.ChangeTime.QuadPart < 0) {
    return failure<ExactHandleMetadata>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成source opened-handle object",
        L"通常file/directory、単一link、named ADSなし、非reparse、固定容量上限を証明できません");
  }

  ExactHandleMetadata result{
      .volume_serial_number = identifier.VolumeSerialNumber,
      .size_bytes = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .allocation_bytes =
          static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
      .creation_time = static_cast<std::uint64_t>(
          basic.CreationTime.QuadPart),
      .last_access_time = static_cast<std::uint64_t>(
          basic.LastAccessTime.QuadPart),
      .last_write_time = static_cast<std::uint64_t>(
          basic.LastWriteTime.QuadPart),
      .change_time = static_cast<std::uint64_t>(
          basic.ChangeTime.QuadPart),
      .file_attributes = attributes.FileAttributes,
      .hard_link_count = standard.NumberOfLinks,
      .reparse_tag = attributes.ReparseTag,
      .named_stream_count = streams.value().named_stream_count,
      .unnamed_stream_count =
          streams.value().unnamed_data_stream_count,
      .directory = directory,
  };
  static_assert(sizeof(result.file_id) == sizeof(identifier.FileId.Identifier));
  std::memcpy(
      result.file_id.data(),
      identifier.FileId.Identifier,
      result.file_id.size());
  return clonecore::Result<ExactHandleMetadata>::success(result);
}

std::optional<std::uint64_t> canonicalize_fat_time(
    const std::uint64_t value) noexcept {
  if (value < kFatEpochFileTime) {
    return std::nullopt;
  }
  const std::uint64_t delta = value - kFatEpochFileTime;
  return kFatEpochFileTime +
      delta / migrationcore::kFileSystemRecreateTimestampQuantum100ns *
          migrationcore::kFileSystemRecreateTimestampQuantum100ns;
}

std::uint32_t portable_attributes(const std::uint32_t attributes) noexcept {
  std::uint32_t result{};
  if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
    result |= migrationcore::recreate_attribute_read_only;
  }
  if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0U) {
    result |= migrationcore::recreate_attribute_hidden;
  }
  if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0U) {
    result |= migrationcore::recreate_attribute_system;
  }
  if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0U) {
    result |= migrationcore::recreate_attribute_archive;
  }
  return result;
}

clonecore::Result<migrationcore::FileSystemRecreateSha256>
hash_opened_file_to_stable_eof(
    const HANDLE file,
    const ExactHandleMetadata& before,
    const migrationcore::MigrationFileSystem file_system,
    const WindowsFileSystemRecreateSourceLimits& limits) {
  LARGE_INTEGER zero{};
  if (SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == FALSE) {
    return clonecore::Result<
        migrationcore::FileSystemRecreateSha256>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"再作成source stream先頭移動",
            GetLastError()));
  }
  Sha256Accumulator hash;
  const auto initialized = hash.initialize();
  if (!initialized) {
    return clonecore::Result<
        migrationcore::FileSystemRecreateSha256>::failure(
        initialized.error());
  }
  std::vector<std::byte> buffer(kHashReadBytes);
  std::uint64_t total{};
  for (;;) {
    DWORD read{};
    if (ReadFile(
            file,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &read,
            nullptr) == FALSE) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateSha256>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"再作成source stable EOF読取り",
              GetLastError()));
    }
    if (read == 0U) {
      break;
    }
    if (total > before.size_bytes || read > before.size_bytes - total) {
      return failure<migrationcore::FileSystemRecreateSha256>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"再作成source stable EOF容量",
          L"opened-handle streamが観測済みEOFを超えました");
    }
    const auto updated = hash.update(
        std::span<const std::byte>(buffer.data(), read));
    if (!updated) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateSha256>::failure(updated.error());
    }
    total += read;
  }
  if (total != before.size_bytes) {
    return failure<migrationcore::FileSystemRecreateSha256>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_HANDLE_EOF,
        L"再作成source stable EOF容量",
        L"opened-handle streamが観測済みEOFより前で終了しました");
  }
  auto digest = hash.finish();
  if (!digest) {
    return digest;
  }
  const auto after = query_exact_handle_metadata(
      file, false, file_system, limits);
  if (!after || !same_exact_metadata(before, after.value())) {
    return after
        ? failure<migrationcore::FileSystemRecreateSha256>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"再作成source FileId/EOF再照合",
              L"SHA-256読取り前後でFileId、size、times、stream状態が変化しました")
        : clonecore::Result<
              migrationcore::FileSystemRecreateSha256>::failure(
              after.error());
  }
  return digest;
}

struct DirectoryChild final {
  std::wstring name;
  std::wstring short_name;
  std::uint32_t attributes{};
  std::int64_t end_of_file{};
  std::int64_t creation_time{};
  std::int64_t last_write_time{};
  std::int64_t file_id{};
};

bool same_directory_child(
    const DirectoryChild& left,
    const DirectoryChild& right) noexcept {
  return left.name == right.name && left.short_name == right.short_name &&
      left.attributes == right.attributes &&
      left.end_of_file == right.end_of_file &&
      left.creation_time == right.creation_time &&
      left.last_write_time == right.last_write_time &&
      left.file_id == right.file_id;
}

clonecore::Result<std::vector<DirectoryChild>> enumerate_directory_handle(
    const HANDLE directory,
    const WindowsFileSystemRecreateSourceLimits& limits) {
  std::vector<DirectoryChild> children;
  std::vector<std::byte> buffer(kDirectoryQueryBytes);
  bool restart = true;
  constexpr std::size_t header_bytes =
      offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
  for (;;) {
    const FILE_INFO_BY_HANDLE_CLASS info_class = restart
        ? FileIdBothDirectoryRestartInfo
        : FileIdBothDirectoryInfo;
    if (GetFileInformationByHandleEx(
            directory,
            info_class,
            buffer.data(),
            static_cast<DWORD>(buffer.size())) == FALSE) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_NO_MORE_FILES) {
        break;
      }
      return clonecore::Result<std::vector<DirectoryChild>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"再作成source handle directory列挙",
              native_code));
    }
    restart = false;
    std::size_t offset{};
    for (;;) {
      if (offset > buffer.size() - header_bytes ||
          offset % alignof(FILE_ID_BOTH_DIR_INFO) != 0U) {
        return failure<std::vector<DirectoryChild>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"再作成source directory entry境界",
            L"directory entry headerがbuffer境界外です");
      }
      const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
          buffer.data() + offset);
      const int short_name_bytes = info->ShortNameLength;
      if ((info->FileNameLength % sizeof(wchar_t)) != 0U ||
          info->FileNameLength > buffer.size() - offset - header_bytes ||
          info->FileNameLength / sizeof(wchar_t) >
              migrationcore::kMaximumFileSystemRecreateComponentUtf16Units ||
          short_name_bytes < 0 ||
          (static_cast<std::size_t>(short_name_bytes) % sizeof(wchar_t)) !=
              0U ||
          static_cast<std::size_t>(short_name_bytes) >
              sizeof(info->ShortName)) {
        return failure<std::vector<DirectoryChild>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_NAME,
            L"再作成source directory entry名",
            L"長名または短名が固定UTF-16上限外です");
      }
      const std::wstring name(
          info->FileName,
          info->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        if (children.size() >= limits.maximum_entries) {
          return failure<std::vector<DirectoryChild>>(
              clonecore::ErrorCode::invalid_data,
              ERROR_BUFFER_OVERFLOW,
              L"再作成source directory entry件数",
              L"1 directoryのentry件数が固定上限を超えました");
        }
        children.push_back(DirectoryChild{
            .name = name,
            .short_name = std::wstring(
                info->ShortName,
                static_cast<std::size_t>(short_name_bytes) /
                    sizeof(wchar_t)),
            .attributes = info->FileAttributes,
            .end_of_file = info->EndOfFile.QuadPart,
            .creation_time = info->CreationTime.QuadPart,
            .last_write_time = info->LastWriteTime.QuadPart,
            .file_id = info->FileId.QuadPart,
        });
      }
      if (info->NextEntryOffset == 0U) {
        break;
      }
      if (info->NextEntryOffset < header_bytes ||
          info->NextEntryOffset > buffer.size() - offset) {
        return failure<std::vector<DirectoryChild>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"再作成source directory entry連鎖",
            L"directory entry offsetがbuffer境界外です");
      }
      offset += info->NextEntryOffset;
    }
  }

  std::map<std::wstring, std::size_t, OrdinalCaseInsensitiveLess> aliases;
  for (std::size_t index = 0U; index < children.size(); ++index) {
    const auto add_alias = [&](const std::wstring& value) {
      if (value.empty()) {
        return true;
      }
      const auto [position, inserted] = aliases.emplace(value, index);
      return inserted || position->second == index;
    };
    if (!add_alias(children[index].name) ||
        !add_alias(children[index].short_name)) {
      return failure<std::vector<DirectoryChild>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DUP_NAME,
          L"再作成source short-name alias衝突",
          L"同じdirectoryで長名または8.3短名aliasが別entryと衝突しています");
    }
  }
  std::sort(
      children.begin(),
      children.end(),
      [](const DirectoryChild& left, const DirectoryChild& right) {
        return OrdinalCaseInsensitiveLess{}(left.name, right.name);
      });
  return clonecore::Result<std::vector<DirectoryChild>>::success(
      std::move(children));
}

bool same_directory_census(
    const std::vector<DirectoryChild>& left,
    const std::vector<DirectoryChild>& right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          same_directory_child);
}

struct ScannedEntry final {
  migrationcore::CanonicalFileSystemTreeEntry canonical;
  ExactHandleMetadata exact;
};

struct TreeScanState final {
  migrationcore::MigrationFileSystem file_system{
      migrationcore::MigrationFileSystem::fat32};
  WindowsFileSystemRecreateSourceLimits limits;
  std::vector<ScannedEntry> entries;
};

clonecore::Status scan_directory_contents(
    const HANDLE directory,
    const std::wstring& directory_path,
    const std::size_t depth,
    TreeScanState& state) {
  if (depth > state.limits.maximum_depth) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"再作成source directory深さ",
        L"完全再帰列挙が固定深さ上限を超えました");
  }
  const auto directory_before = query_exact_handle_metadata(
      directory, true, state.file_system, state.limits);
  if (!directory_before) {
    return clonecore::Status::failure(directory_before.error());
  }
  const auto children = enumerate_directory_handle(directory, state.limits);
  if (!children) {
    return clonecore::Status::failure(children.error());
  }

  for (const auto& child : children.value()) {
    if (depth >= state.limits.maximum_depth) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"再作成source directory深さ",
          L"entryのcomponent深さが固定上限を超えました");
    }
    if (state.entries.size() >= state.limits.maximum_entries) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"再作成source tree件数",
          L"完全再帰列挙が固定entry件数上限を超えました");
    }
    if (child.name.empty() || child.name == L"." || child.name == L".." ||
        child.name.find_first_of(L"\\/") != child.name.npos ||
        (child.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"再作成source child namespace",
          L"禁止componentまたはreparse objectを完全列挙で検出しました");
    }
    const std::wstring relative_path = directory_path.empty()
        ? child.name
        : directory_path + L"\\" + child.name;
    if (relative_path.size() > state.limits.maximum_path_utf16_units) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_FILENAME_EXCED_RANGE,
          L"再作成source relative path",
          L"root相対pathが固定UTF-16上限を超えました");
    }
    const bool child_directory =
        (child.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    auto opened = open_relative_object_read_only(
        directory, child.name, child_directory);
    if (!opened) {
      return clonecore::Status::failure(opened.error());
    }
    const auto before = query_exact_handle_metadata(
        opened.value().get(),
        child_directory,
        state.file_system,
        state.limits);
    if (!before) {
      return clonecore::Status::failure(before.error());
    }
    const auto creation = canonicalize_fat_time(before.value().creation_time);
    const auto write = canonicalize_fat_time(before.value().last_write_time);
    if (!creation.has_value() || !write.has_value()) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"再作成source portable timestamp",
          L"FAT/exFAT共通精度へ正規化できない時刻を検出しました");
    }

    migrationcore::FileSystemRecreateSha256 digest{};
    if (!child_directory) {
      auto hashed = hash_opened_file_to_stable_eof(
          opened.value().get(),
          before.value(),
          state.file_system,
          state.limits);
      if (!hashed) {
        return clonecore::Status::failure(hashed.error());
      }
      digest = hashed.take_value();
    }
    state.entries.push_back(ScannedEntry{
        .canonical = migrationcore::CanonicalFileSystemTreeEntry{
            .relative_path = relative_path,
            .kind = child_directory
                ? migrationcore::FileSystemRecreateEntryKind::directory
                : migrationcore::FileSystemRecreateEntryKind::regular_file,
            .size_bytes = child_directory ? 0U : before.value().size_bytes,
            .portable_attributes =
                portable_attributes(before.value().file_attributes),
            .creation_time_utc_100ns = creation.value(),
            .last_write_time_utc_100ns = write.value(),
            .content_sha256 = digest,
            .hard_link_count = before.value().hard_link_count,
            .alternate_data_stream_count =
                before.value().named_stream_count,
            .reparse_tag = before.value().reparse_tag,
            .opened_handle_identity_stable = true,
            .unnamed_stream_hashed_to_stable_eof = !child_directory,
            .namespace_supported = true,
        },
        .exact = before.value(),
    });

    if (child_directory) {
      const auto scanned = scan_directory_contents(
          opened.value().get(),
          relative_path,
          depth + 1U,
          state);
      if (!scanned) {
        return scanned;
      }
    }
    const auto after = query_exact_handle_metadata(
        opened.value().get(),
        child_directory,
        state.file_system,
        state.limits);
    if (!after || !same_exact_metadata(before.value(), after.value())) {
      return after
          ? status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_REVISION_MISMATCH,
                L"再作成source entry再照合",
                L"列挙前後でentry FileId、size、times、stream状態が変化しました")
          : clonecore::Status::failure(after.error());
    }
  }

  const auto children_after = enumerate_directory_handle(
      directory, state.limits);
  if (!children_after ||
      !same_directory_census(children.value(), children_after.value())) {
    return children_after
        ? status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"再作成source directory再列挙",
              L"handle-bound完全列挙中にdirectory censusが変化しました")
        : clonecore::Status::failure(children_after.error());
  }
  const auto directory_after = query_exact_handle_metadata(
      directory, true, state.file_system, state.limits);
  if (!directory_after ||
      !same_exact_metadata(directory_before.value(), directory_after.value())) {
    return directory_after
        ? status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"再作成source directory metadata再照合",
              L"完全再帰列挙中にdirectory FileIdまたはmetadataが変化しました")
        : clonecore::Status::failure(directory_after.error());
  }
  return clonecore::success_status();
}

struct SourceExtent final {
  std::uint32_t disk_number{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
};

clonecore::Result<SourceExtent> query_single_volume_extent(
    const HANDLE root) {
  VOLUME_DISK_EXTENTS extents{};
  DWORD returned{};
  if (DeviceIoControl(
          root,
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0U,
          &extents,
          sizeof(extents),
          &returned,
          nullptr) == FALSE) {
    return clonecore::Result<SourceExtent>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"再作成source Volume extent query",
            GetLastError()));
  }
  if (returned < sizeof(VOLUME_DISK_EXTENTS) ||
      extents.NumberOfDiskExtents != 1U ||
      extents.Extents[0].StartingOffset.QuadPart < 0 ||
      extents.Extents[0].ExtentLength.QuadPart <= 0) {
    return failure<SourceExtent>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成source single extent",
        L"source rootを1つの正のphysical partition extentへ束縛できません");
  }
  return clonecore::Result<SourceExtent>::success(SourceExtent{
      .disk_number = extents.Extents[0].DiskNumber,
      .offset_bytes = static_cast<std::uint64_t>(
          extents.Extents[0].StartingOffset.QuadPart),
      .length_bytes = static_cast<std::uint64_t>(
          extents.Extents[0].ExtentLength.QuadPart),
  });
}

struct RootIdentity final {
  FileId128 file_id{};
  std::uint64_t volume_serial_number{};
  std::uint64_t creation_time{};
  std::uint64_t last_write_time{};
  std::uint32_t attributes{};
  std::uint32_t reparse_tag{};
};

bool same_root_identity(
    const RootIdentity& left,
    const RootIdentity& right) noexcept {
  return left.file_id == right.file_id &&
      left.volume_serial_number == right.volume_serial_number &&
      left.creation_time == right.creation_time &&
      left.last_write_time == right.last_write_time &&
      left.attributes == right.attributes &&
      left.reparse_tag == right.reparse_tag;
}

clonecore::Result<RootIdentity> query_root_identity(const HANDLE root) {
  FILE_ID_INFO identifier{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (GetFileInformationByHandleEx(
          root, FileIdInfo, &identifier, sizeof(identifier)) == FALSE ||
      GetFileInformationByHandleEx(
          root, FileBasicInfo, &basic, sizeof(basic)) == FALSE ||
      GetFileInformationByHandleEx(
          root, FileStandardInfo, &standard, sizeof(standard)) == FALSE ||
      GetFileInformationByHandleEx(
          root,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE) {
    return clonecore::Result<RootIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"再作成source root FileId",
            GetLastError()));
  }
  if (standard.Directory == FALSE || standard.DeletePending != FALSE ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      attributes.ReparseTag != 0U || basic.CreationTime.QuadPart < 0 ||
      basic.LastWriteTime.QuadPart < 0) {
    return failure<RootIdentity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成source root object",
        L"source rootが通常directory・非reparseとして証明できません");
  }
  RootIdentity result{
      .volume_serial_number = identifier.VolumeSerialNumber,
      .creation_time = static_cast<std::uint64_t>(
          basic.CreationTime.QuadPart),
      .last_write_time = static_cast<std::uint64_t>(
          basic.LastWriteTime.QuadPart),
      .attributes = attributes.FileAttributes,
      .reparse_tag = attributes.ReparseTag,
  };
  static_assert(sizeof(result.file_id) == sizeof(identifier.FileId.Identifier));
  std::memcpy(
      result.file_id.data(),
      identifier.FileId.Identifier,
      result.file_id.size());
  return clonecore::Result<RootIdentity>::success(result);
}

clonecore::Result<migrationcore::MigrationFileSystem>
query_root_file_system(const HANDLE root) {
  std::array<wchar_t, 32U> file_system{};
  DWORD serial{};
  DWORD maximum_component{};
  DWORD flags{};
  if (GetVolumeInformationByHandleW(
          root,
          nullptr,
          0U,
          &serial,
          &maximum_component,
          &flags,
          file_system.data(),
          static_cast<DWORD>(file_system.size())) == FALSE) {
    return clonecore::Result<migrationcore::MigrationFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"再作成source filesystem query",
            GetLastError()));
  }
  if (same_text_case_insensitive(file_system.data(), L"FAT32")) {
    return clonecore::Result<migrationcore::MigrationFileSystem>::success(
        migrationcore::MigrationFileSystem::fat32);
  }
  if (same_text_case_insensitive(file_system.data(), L"exFAT")) {
    return clonecore::Result<migrationcore::MigrationFileSystem>::success(
        migrationcore::MigrationFileSystem::exfat);
  }
  return failure<migrationcore::MigrationFileSystem>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"再作成source filesystem",
      L"target-only再構成sourceはFAT32またはexFATに限定されます");
}

clonecore::Result<migrationcore::FileSystemRecreateSha256>
observe_epoch_token(
    const WindowsFileSystemRecreateSourceRequest& request) {
  try {
    if (!request.observe_source_epoch_token) {
      return failure<migrationcore::FileSystemRecreateSha256>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"再作成source epoch observer",
          L"VSS/read-only epoch tokenのfresh observerがありません");
    }
    auto observed = request.observe_source_epoch_token();
    if (!observed) {
      return observed;
    }
    if (observed.value() !=
        request.expected_source_epoch_token_sha256) {
      return failure<migrationcore::FileSystemRecreateSha256>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"再作成source epoch token再識別",
          L"VSS/read-only source tokenが固定済みepochと一致しません");
    }
    return observed;
  } catch (...) {
    return failure<migrationcore::FileSystemRecreateSha256>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"再作成source epoch observer",
        L"source epoch observerから例外が発生しました");
  }
}

struct ObservedSourceBinding final {
  clonecore::StableDiskIdentity disk;
  RootIdentity root;
  SourceExtent extent;
  migrationcore::MigrationFileSystem file_system{
      migrationcore::MigrationFileSystem::fat32};
  migrationcore::FileSystemRecreateSha256 source_token{};
  migrationcore::FileSystemRecreateSha256 enumeration_epoch{};
};

bool same_source_binding(
    const ObservedSourceBinding& left,
    const ObservedSourceBinding& right) noexcept {
  return left.disk.model == right.disk.model &&
      left.disk.size_bytes == right.disk.size_bytes &&
      left.disk.logical_sector_size == right.disk.logical_sector_size &&
      left.disk.serial_suffix == right.disk.serial_suffix &&
      left.disk.device_instance_id == right.disk.device_instance_id &&
      same_root_identity(left.root, right.root) &&
      left.extent.disk_number == right.extent.disk_number &&
      left.extent.offset_bytes == right.extent.offset_bytes &&
      left.extent.length_bytes == right.extent.length_bytes &&
      left.file_system == right.file_system &&
      left.source_token == right.source_token &&
      left.enumeration_epoch == right.enumeration_epoch;
}

clonecore::Result<migrationcore::FileSystemRecreateSha256>
hash_source_epoch_binding(
    const WindowsFileSystemRecreateSourceRequest& request,
    const clonecore::StableDiskIdentity& disk,
    const RootIdentity& root,
    const SourceExtent& extent,
    const WindowsFileSystemRecreateSourceRootKind root_kind,
    const migrationcore::FileSystemRecreateSha256& source_token) {
  std::vector<std::byte> bytes;
  bytes.reserve(768U);
  if (!append_narrow(bytes, "Y-TEC-FS-RECREATE-SOURCE-EPOCH-V1") ||
      !append_stable_identity(bytes, disk) ||
      !append_wide(bytes, normalized_root_path(request.source_root_path))) {
    return failure<migrationcore::FileSystemRecreateSha256>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成source epoch Hash",
        L"source binding文字列がHash形式上限を超えています");
  }
  append_u8(bytes, static_cast<std::uint8_t>(root_kind));
  append_u32(bytes, request.source_table_index);
  append_u32(bytes, extent.disk_number);
  append_u64(bytes, extent.offset_bytes);
  append_u64(bytes, extent.length_bytes);
  append_u64(bytes, root.volume_serial_number);
  bytes.insert(bytes.end(), root.file_id.begin(), root.file_id.end());
  append_u64(bytes, root.creation_time);
  append_u64(bytes, root.last_write_time);
  append_digest(bytes, source_token);
  return imageformat::sha256(bytes);
}

clonecore::Result<ObservedSourceBinding> observe_source_binding(
    const WindowsFileSystemRecreateSourceRequest& request,
    const WindowsFileSystemRecreateSourceRootKind root_kind,
    const HANDLE root) {
  auto token = observe_epoch_token(request);
  if (!token) {
    return clonecore::Result<ObservedSourceBinding>::failure(token.error());
  }
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (!inventory) {
    return failure<ObservedSourceBinding>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"再作成source disk inventory",
        L"read-only disk inventory providerを作成できませんでした");
  }
  auto disk = diskmodel::reidentify_read_only_physical_disk(
      request.expected_source_disk, *inventory);
  if (!disk) {
    return clonecore::Result<ObservedSourceBinding>::failure(disk.error());
  }
  const auto partition = std::find_if(
      disk.value().observed.partitions.begin(),
      disk.value().observed.partitions.end(),
      [&](const diskmodel::PartitionInfo& candidate) {
        return candidate.number == request.source_table_index;
      });
  if (partition == disk.value().observed.partitions.end() ||
      partition->offset_bytes != request.source_partition_offset_bytes ||
      partition->size_bytes != request.source_partition_length_bytes) {
    return failure<ObservedSourceBinding>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"再作成source partition extent再識別",
        L"read-only inventoryの区画番号・offset・lengthが固定値と一致しません");
  }

  auto extent = query_single_volume_extent(root);
  if (!extent) {
    return clonecore::Result<ObservedSourceBinding>::failure(extent.error());
  }
  if (extent.value().disk_number != disk.value().observed.disk_number ||
      extent.value().offset_bytes != request.source_partition_offset_bytes ||
      extent.value().length_bytes != request.source_partition_length_bytes) {
    return failure<ObservedSourceBinding>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"再作成source root extent再識別",
        L"source rootが固定済みphysical diskの完全一致single extentではありません");
  }
  auto root_identity = query_root_identity(root);
  if (!root_identity) {
    return clonecore::Result<ObservedSourceBinding>::failure(
        root_identity.error());
  }
  auto file_system = query_root_file_system(root);
  if (!file_system) {
    return clonecore::Result<ObservedSourceBinding>::failure(
        file_system.error());
  }
  if (file_system.value() != request.expected_file_system) {
    return failure<ObservedSourceBinding>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"再作成source filesystem再識別",
        L"opened rootのFAT32/exFAT形式が固定済み計画と一致しません");
  }
  auto epoch = hash_source_epoch_binding(
      request,
      disk.value().identity,
      root_identity.value(),
      extent.value(),
      root_kind,
      token.value());
  if (!epoch) {
    return clonecore::Result<ObservedSourceBinding>::failure(epoch.error());
  }
  return clonecore::Result<ObservedSourceBinding>::success(
      ObservedSourceBinding{
          .disk = disk.value().identity,
          .root = root_identity.value(),
          .extent = extent.value(),
          .file_system = file_system.value(),
          .source_token = token.value(),
          .enumeration_epoch = epoch.value(),
      });
}

bool canonical_entry_equal(
    const migrationcore::CanonicalFileSystemTreeEntry& left,
    const migrationcore::CanonicalFileSystemTreeEntry& right) noexcept {
  return left.relative_path == right.relative_path &&
      left.kind == right.kind && left.size_bytes == right.size_bytes &&
      left.portable_attributes == right.portable_attributes &&
      left.creation_time_utc_100ns == right.creation_time_utc_100ns &&
      left.last_write_time_utc_100ns == right.last_write_time_utc_100ns &&
      left.content_sha256 == right.content_sha256 &&
      left.hard_link_count == right.hard_link_count &&
      left.alternate_data_stream_count ==
          right.alternate_data_stream_count &&
      left.reparse_tag == right.reparse_tag &&
      left.opened_handle_identity_stable ==
          right.opened_handle_identity_stable &&
      left.unnamed_stream_hashed_to_stable_eof ==
          right.unnamed_stream_hashed_to_stable_eof &&
      left.namespace_supported == right.namespace_supported;
}

bool geometry_equal(
    const migrationcore::FileSystemRecreateFormatGeometry& left,
    const migrationcore::FileSystemRecreateFormatGeometry& right) noexcept {
  return left.file_system == right.file_system &&
      left.target_volume_bytes == right.target_volume_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.cluster_size == right.cluster_size &&
      left.maximum_path_utf16_units == right.maximum_path_utf16_units &&
      left.maximum_component_utf16_units ==
          right.maximum_component_utf16_units;
}

clonecore::Result<clonecore::UniqueHandle> reopen_file_from_root(
    const HANDLE root,
    const std::wstring& relative_path,
    const migrationcore::MigrationFileSystem file_system,
    const WindowsFileSystemRecreateSourceLimits& limits) {
  if (relative_path.empty() || relative_path.front() == L'\\' ||
      relative_path.back() == L'\\') {
    return failure<clonecore::UniqueHandle>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"再作成source file再open path",
        L"canonical root相対pathではありません");
  }
  HANDLE parent = root;
  std::vector<clonecore::UniqueHandle> directory_handles;
  std::size_t begin{};
  for (;;) {
    const std::size_t separator = relative_path.find(L'\\', begin);
    const bool final = separator == std::wstring::npos;
    const std::size_t end = final ? relative_path.size() : separator;
    const std::wstring_view component(relative_path.data() + begin, end - begin);
    auto opened = open_relative_object_read_only(parent, component, !final);
    if (!opened) {
      return clonecore::Result<clonecore::UniqueHandle>::failure(
          opened.error());
    }
    if (final) {
      return opened;
    }
    const auto metadata = query_exact_handle_metadata(
        opened.value().get(), true, file_system, limits);
    if (!metadata) {
      return clonecore::Result<clonecore::UniqueHandle>::failure(
          metadata.error());
    }
    directory_handles.push_back(opened.take_value());
    parent = directory_handles.back().get();
    begin = separator + 1U;
  }
}

class WindowsFileSystemRecreateSourceFile final
    : public IFileSystemRecreateSourceFile {
 public:
  WindowsFileSystemRecreateSourceFile(
      clonecore::UniqueHandle handle,
      ExactHandleMetadata exact,
      migrationcore::CanonicalFileSystemTreeEntry expected,
      const migrationcore::MigrationFileSystem file_system,
      WindowsFileSystemRecreateSourceLimits limits,
      std::unique_ptr<Sha256Accumulator> hash)
      : handle_(std::move(handle)),
        exact_(std::move(exact)),
        expected_(std::move(expected)),
        file_system_(file_system),
        limits_(limits),
        hash_(std::move(hash)) {}

  [[nodiscard]] std::uint64_t expected_size_bytes() const noexcept override {
    return expected_.size_bytes;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_next(
      const std::size_t maximum_bytes) override {
    if (finished_ || failed_ || eof_observed_ || maximum_bytes == 0U ||
        maximum_bytes > kDefaultWindowsFileSystemRecreateTransferBytes ||
        total_read_ > expected_.size_bytes) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"再作成source execution stream読取り",
          L"stream状態または要求chunk上限が不正です");
    }
    if (total_read_ == expected_.size_bytes) {
      std::byte extra{};
      DWORD read{};
      if (ReadFile(handle_.get(), &extra, 1U, &read, nullptr) == FALSE) {
        failed_ = true;
        return clonecore::Result<std::vector<std::byte>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"再作成source execution EOF probe",
                GetLastError()));
      }
      if (read != 0U) {
        failed_ = true;
        return failure<std::vector<std::byte>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_REVISION_MISMATCH,
            L"再作成source execution stable EOF",
            L"再openしたsource fileが固定済みEOFを超えています");
      }
      eof_observed_ = true;
      return clonecore::Result<std::vector<std::byte>>::success({});
    }

    const std::uint64_t remaining = expected_.size_bytes - total_read_;
    const std::size_t requested = static_cast<std::size_t>((std::min)(
        remaining, static_cast<std::uint64_t>(maximum_bytes)));
    std::vector<std::byte> bytes(requested);
    DWORD read{};
    if (ReadFile(
            handle_.get(),
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &read,
            nullptr) == FALSE) {
      failed_ = true;
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"再作成source execution stream読取り",
              GetLastError()));
    }
    if (read == 0U || read > bytes.size()) {
      failed_ = true;
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_HANDLE_EOF,
          L"再作成source execution stream short EOF",
          L"再openしたsource fileが固定済みsizeより前で終了しました");
    }
    bytes.resize(read);
    const auto updated = hash_->update(bytes);
    if (!updated) {
      failed_ = true;
      return clonecore::Result<std::vector<std::byte>>::failure(
          updated.error());
    }
    total_read_ += read;
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  [[nodiscard]] clonecore::Status finish_and_verify() override {
    if (finished_ || failed_ || !eof_observed_ ||
        total_read_ != expected_.size_bytes || hash_ == nullptr) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"再作成source execution stream完了",
          L"exact EOFまでの逐次読取りが完了していません");
    }
    auto digest = hash_->finish();
    if (!digest) {
      failed_ = true;
      return clonecore::Status::failure(digest.error());
    }
    const auto after = query_exact_handle_metadata(
        handle_.get(), false, file_system_, limits_);
    if (!after || !same_exact_metadata(exact_, after.value()) ||
        digest.value() != expected_.content_sha256) {
      failed_ = true;
      return after
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"再作成source execution FileId/Hash再照合",
                L"FileId、size、times、stable EOF、またはSHA-256が観測時と一致しません")
          : clonecore::Status::failure(after.error());
    }
    finished_ = true;
    return clonecore::success_status();
  }

 private:
  clonecore::UniqueHandle handle_;
  ExactHandleMetadata exact_;
  migrationcore::CanonicalFileSystemTreeEntry expected_;
  migrationcore::MigrationFileSystem file_system_;
  WindowsFileSystemRecreateSourceLimits limits_;
  std::unique_ptr<Sha256Accumulator> hash_;
  std::uint64_t total_read_{};
  bool eof_observed_{};
  bool finished_{};
  bool failed_{};
};

class WindowsFileSystemRecreateSourceSession final
    : public IFileSystemRecreateSourceSession {
 public:
  WindowsFileSystemRecreateSourceSession(
      WindowsFileSystemRecreateSourceRequest request,
      const WindowsFileSystemRecreateSourceRootKind root_kind,
      clonecore::UniqueHandle root,
      ObservedSourceBinding initial,
      migrationcore::CanonicalFileSystemTree tree,
      std::vector<ExactHandleMetadata> exact_entries)
      : request_(std::move(request)),
        root_kind_(root_kind),
        root_(std::move(root)),
        initial_(std::move(initial)),
        tree_(std::move(tree)),
        exact_entries_(std::move(exact_entries)) {}

  [[nodiscard]] const clonecore::StableDiskIdentity& source_disk()
      const noexcept override {
    return initial_.disk;
  }

  [[nodiscard]] std::uint32_t source_table_index() const noexcept override {
    return request_.source_table_index;
  }

  [[nodiscard]] std::uint64_t source_partition_offset_bytes()
      const noexcept override {
    return request_.source_partition_offset_bytes;
  }

  [[nodiscard]] std::uint64_t source_partition_length_bytes()
      const noexcept override {
    return request_.source_partition_length_bytes;
  }

  [[nodiscard]] const migrationcore::CanonicalFileSystemTree&
  canonical_tree() const noexcept override {
    return tree_;
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateSourceEpochEvidence>
  revalidate_source_epoch() override {
    auto observed = observe_source_binding(
        request_, root_kind_, root_.get());
    if (!observed) {
      return clonecore::Result<
          FileSystemRecreateSourceEpochEvidence>::failure(observed.error());
    }
    if (!same_source_binding(initial_, observed.value()) ||
        observed.value().enumeration_epoch !=
            tree_.enumeration_epoch_sha256) {
      return failure<FileSystemRecreateSourceEpochEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"再作成source session epoch再識別",
          L"source disk、root FileId、extent、filesystem、またはepoch tokenが変化しました");
    }
    if (freshness_sequence_ ==
        (std::numeric_limits<std::uint64_t>::max)()) {
      return failure<FileSystemRecreateSourceEpochEvidence>(
          clonecore::ErrorCode::internal_error,
          ERROR_ARITHMETIC_OVERFLOW,
          L"再作成source freshness sequence",
          L"source再識別回数が表現上限を超えました");
    }
    ++freshness_sequence_;
    return clonecore::Result<
        FileSystemRecreateSourceEpochEvidence>::success(
        FileSystemRecreateSourceEpochEvidence{
            .observed_source_disk = observed.value().disk,
            .enumeration_epoch_sha256 =
                observed.value().enumeration_epoch,
            .source_table_index = request_.source_table_index,
            .source_partition_offset_bytes =
                observed.value().extent.offset_bytes,
            .source_partition_length_bytes =
                observed.value().extent.length_bytes,
            .freshness_sequence = freshness_sequence_,
            .root_file_id_stable = true,
            .exact_single_extent = true,
            .source_token_reidentified = true,
        });
  }

  [[nodiscard]] clonecore::Result<
      std::unique_ptr<IFileSystemRecreateSourceFile>>
  open_regular_file(
      const std::size_t canonical_entry_index,
      const migrationcore::CanonicalFileSystemTreeEntry& expected_entry)
      override {
    if (canonical_entry_index >= tree_.entries.size() ||
        canonical_entry_index >= exact_entries_.size() ||
        expected_entry.kind !=
            migrationcore::FileSystemRecreateEntryKind::regular_file ||
        !canonical_entry_equal(
            tree_.entries[canonical_entry_index], expected_entry)) {
      return failure<std::unique_ptr<IFileSystemRecreateSourceFile>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"再作成source execution entry binding",
          L"実行計画entryが観測済みcanonical entryと完全一致しません");
    }
    auto reopened = reopen_file_from_root(
        root_.get(),
        expected_entry.relative_path,
        request_.expected_file_system,
        request_.limits);
    if (!reopened) {
      return clonecore::Result<
          std::unique_ptr<IFileSystemRecreateSourceFile>>::failure(
          reopened.error());
    }
    const auto exact = query_exact_handle_metadata(
        reopened.value().get(),
        false,
        request_.expected_file_system,
        request_.limits);
    if (!exact ||
        !same_exact_metadata(
            exact_entries_[canonical_entry_index], exact.value())) {
      return exact
          ? failure<std::unique_ptr<IFileSystemRecreateSourceFile>>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_REVISION_MISMATCH,
                L"再作成source execution file再open",
                L"再openしたfileのFileId、size、times、link、stream状態が観測時と一致しません")
          : clonecore::Result<
                std::unique_ptr<IFileSystemRecreateSourceFile>>::failure(
                exact.error());
    }
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(
            reopened.value().get(), zero, nullptr, FILE_BEGIN) == FALSE) {
      return clonecore::Result<
          std::unique_ptr<IFileSystemRecreateSourceFile>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"再作成source execution file先頭",
              GetLastError()));
    }
    auto hash = std::make_unique<Sha256Accumulator>();
    const auto initialized = hash->initialize();
    if (!initialized) {
      return clonecore::Result<
          std::unique_ptr<IFileSystemRecreateSourceFile>>::failure(
          initialized.error());
    }
    return clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceFile>>::success(
        std::make_unique<WindowsFileSystemRecreateSourceFile>(
            reopened.take_value(),
            exact.value(),
            expected_entry,
            request_.expected_file_system,
            request_.limits,
            std::move(hash)));
  }

 private:
  WindowsFileSystemRecreateSourceRequest request_;
  WindowsFileSystemRecreateSourceRootKind root_kind_;
  clonecore::UniqueHandle root_;
  ObservedSourceBinding initial_;
  migrationcore::CanonicalFileSystemTree tree_;
  std::vector<ExactHandleMetadata> exact_entries_;
  std::uint64_t freshness_sequence_{};
};

clonecore::Status validate_source_request(
    const WindowsFileSystemRecreateSourceRequest& request) {
  const auto identity = clonecore::validate_stable_identity(
      request.expected_source_disk,
      request.expected_source_disk,
      L"再作成コピー元");
  if (!identity) {
    return identity;
  }
  if (!is_supported_recreate_file_system(request.expected_file_system) ||
      request.source_table_index == 0U ||
      request.source_partition_offset_bytes == 0U ||
      request.source_partition_length_bytes == 0U ||
      (request.expected_source_disk.logical_sector_size != 512U &&
       request.expected_source_disk.logical_sector_size != 4096U) ||
      request.source_partition_offset_bytes >
          request.expected_source_disk.size_bytes ||
      request.source_partition_length_bytes >
          request.expected_source_disk.size_bytes -
              request.source_partition_offset_bytes ||
      request.source_partition_offset_bytes %
              request.expected_source_disk.logical_sector_size !=
          0U ||
      request.source_partition_length_bytes %
              request.expected_source_disk.logical_sector_size !=
          0U ||
      !classify_source_root(request.source_root_path).has_value() ||
      is_zero_digest(request.expected_source_epoch_token_sha256) ||
      !request.observe_source_epoch_token ||
      request.limits.maximum_entries == 0U ||
      request.limits.maximum_entries >
          migrationcore::kMaximumFileSystemRecreateEntries ||
      request.limits.maximum_depth == 0U ||
      request.limits.maximum_depth >
          request.limits.maximum_path_utf16_units ||
      request.limits.maximum_path_utf16_units == 0U ||
      request.limits.maximum_path_utf16_units >
          migrationcore::kMaximumFileSystemRecreatePathUtf16Units ||
      request.limits.maximum_file_bytes == 0U ||
      request.limits.maximum_file_bytes >
          migrationcore::kExfatMaximumRecreatedFileBytes ||
      request.limits.maximum_stream_query_bytes < 4096U ||
      request.limits.maximum_stream_query_bytes >
          kMaximumWindowsFileSystemRecreateStreamQueryBytes) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"再作成source request",
        L"FAT32/exFAT、512/4096-byte logical sector、canonical root、exact extent、fresh epoch observer、または固定boundsが不正です");
  }
  return clonecore::success_status();
}

clonecore::Status validate_source_matches_core_plan(
    const migrationcore::FileSystemRecreatePlan& plan,
    const IFileSystemRecreateSourceSession& source) {
  const auto& tree = source.canonical_tree();
  if (!is_supported_recreate_file_system(
          plan.target_geometry().file_system) ||
      tree.file_system != plan.target_geometry().file_system ||
      tree.source_table_index != plan.source_table_index() ||
      source.source_table_index() != plan.source_table_index() ||
      tree.enumeration_epoch_sha256 != plan.source_epoch_sha256() ||
      source.source_partition_offset_bytes() == 0U ||
      source.source_partition_length_bytes() == 0U ||
      tree.entries.size() != plan.entries().size()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_REVISION_MISMATCH,
        L"再作成source/core plan binding",
        L"source形式、区画番号、extent、epoch、またはentry件数がpure core planと一致しません");
  }
  for (std::size_t index = 0U; index < tree.entries.size(); ++index) {
    if (!canonical_entry_equal(tree.entries[index], plan.entries()[index])) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"再作成source/core entry binding",
          L"source canonical entryがpure core planと完全一致しません");
    }
  }
  const auto manifest = migrationcore::hash_canonical_file_system_tree(
      tree, plan.target_geometry());
  if (!manifest || manifest.value() != plan.canonical_manifest_sha256()) {
    return manifest
        ? status_failure(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"再作成source/core manifest binding",
              L"source treeの正規manifestがpure core planと一致しません")
        : clonecore::Status::failure(manifest.error());
  }
  return clonecore::success_status();
}

bool guard_equal(
    const FileSystemRecreateMutationGuard& left,
    const FileSystemRecreateMutationGuard& right) noexcept {
  return left.execution_plan_sha256 == right.execution_plan_sha256 &&
      left.source_epoch_sha256 == right.source_epoch_sha256 &&
      left.target_binding_sha256 == right.target_binding_sha256;
}

clonecore::Status validate_mutation_evidence(
    const FileSystemRecreateMutationEvidence& evidence,
    const FileSystemRecreateMutationGuard& expected,
    const FileSystemRecreateTargetIsolationState expected_isolation,
    const bool require_flush) {
  if (!guard_equal(evidence.accepted_guard, expected) ||
      !evidence.guard_revalidated_inside_adapter ||
      !evidence.exact_target_handle_retained ||
      evidence.isolation_state != expected_isolation ||
      !evidence.completion_incomplete ||
      (require_flush && !evidence.flushed)) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"再作成target mutation証跡",
        L"実行計画/source epoch/target binding、exact handle、offline、incomplete、またはflush証跡が不足しています");
  }
  return clonecore::success_status();
}

clonecore::Status validate_source_epoch_evidence(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const IFileSystemRecreateSourceSession& source,
    const FileSystemRecreateSourceEpochEvidence& evidence,
    const std::uint64_t previous_sequence) {
  const auto identity = clonecore::validate_stable_identity(
      plan.source_disk(), evidence.observed_source_disk, L"再作成コピー元");
  if (!identity) {
    return identity;
  }
  if (evidence.enumeration_epoch_sha256 !=
          plan.core_plan().source_epoch_sha256() ||
      evidence.source_table_index != plan.core_plan().source_table_index() ||
      evidence.source_table_index != source.source_table_index() ||
      evidence.source_partition_offset_bytes !=
          source.source_partition_offset_bytes() ||
      evidence.source_partition_length_bytes !=
          source.source_partition_length_bytes() ||
      evidence.freshness_sequence == 0U ||
      evidence.freshness_sequence <= previous_sequence ||
      !evidence.root_file_id_stable || !evidence.exact_single_extent ||
      !evidence.source_token_reidentified) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_REVISION_MISMATCH,
        L"再作成source mutation前再識別",
        L"source stable identity、FileId、extent、epoch token、またはfreshness証跡が固定計画と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_target_observation(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const FileSystemRecreateTargetObservation& observation,
    const std::uint64_t previous_sequence,
    const FileSystemRecreateTargetIsolationState expected_isolation,
    const bool require_exact_target_handle) {
  const auto identity = clonecore::validate_stable_identity(
      plan.target().expected_target_disk,
      observation.observed_target_disk,
      L"再作成コピー先");
  if (!identity) {
    return identity;
  }
  if (same_stable_device(
          plan.source_disk(), observation.observed_target_disk)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"再作成source/target分離",
        L"コピー元とコピー先が同一physical diskを示しています");
  }
  const bool offline_safe = expected_isolation ==
          FileSystemRecreateTargetIsolationState::physical_disk_offline &&
      observation.reviewed_extent_within_disk &&
      !observation.root_file_id_stable &&
      !observation.root_is_non_reparse;
  const bool construction_safe = expected_isolation ==
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive &&
      observation.reviewed_extent_within_disk &&
      observation.exact_partition_extent &&
      observation.exact_target_handle_retained &&
      observation.root_file_id_stable && observation.root_is_non_reparse;
  if (observation.observed_target_disk.is_system_disk ||
      observation.active_rescue_media ||
      observation.target_partition_number !=
          plan.target().target_partition_number ||
      observation.target_partition_offset_bytes !=
          plan.target().target_partition_offset_bytes ||
      observation.target_partition_length_bytes !=
          plan.target().target_partition_length_bytes ||
      observation.freshness_sequence == 0U ||
      observation.freshness_sequence <= previous_sequence ||
      !observation.freshly_reidentified ||
      observation.isolation_state != expected_isolation ||
      (require_exact_target_handle &&
       !observation.exact_target_handle_retained) ||
      (!offline_safe && !construction_safe)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"再作成target mutation前再識別",
        L"target stable identity、exact extent、fresh offline状態、またはactive rescue保護が固定計画と一致しません");
  }
  const auto binding = hash_target_binding(
      observation.observed_target_disk,
      observation.target_partition_number,
      observation.target_partition_offset_bytes,
      observation.target_partition_length_bytes);
  if (!binding || binding.value() != plan.target_binding_sha256()) {
    return binding
        ? status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_REVISION_MISMATCH,
              L"再作成target binding Hash再照合",
              L"fresh target observationが固定済みtarget binding Hashと一致しません")
        : clonecore::Status::failure(binding.error());
  }
  return clonecore::success_status();
}

clonecore::Result<FileSystemRecreateMutationGuard>
revalidate_mutation_boundary(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    IFileSystemRecreateSourceSession& source,
    IFileSystemRecreateTargetPlatform& target,
    std::uint64_t& source_sequence,
    std::uint64_t& target_sequence,
    std::uint64_t& target_reidentification_count,
    const FileSystemRecreateTargetIsolationState expected_isolation,
    const bool require_exact_target_handle) {
  auto source_observed = source.revalidate_source_epoch();
  if (!source_observed) {
    return clonecore::Result<FileSystemRecreateMutationGuard>::failure(
        source_observed.error());
  }
  const auto source_valid = validate_source_epoch_evidence(
      plan, source, source_observed.value(), source_sequence);
  if (!source_valid) {
    return clonecore::Result<FileSystemRecreateMutationGuard>::failure(
        source_valid.error());
  }
  source_sequence = source_observed.value().freshness_sequence;

  auto target_observed = target.reidentify_target_read_only();
  if (!target_observed) {
    return clonecore::Result<FileSystemRecreateMutationGuard>::failure(
        target_observed.error());
  }
  const auto target_valid = validate_target_observation(
      plan,
      target_observed.value(),
      target_sequence,
      expected_isolation,
      require_exact_target_handle);
  if (!target_valid) {
    return clonecore::Result<FileSystemRecreateMutationGuard>::failure(
        target_valid.error());
  }
  target_sequence = target_observed.value().freshness_sequence;
  if (target_reidentification_count ==
      (std::numeric_limits<std::uint64_t>::max)()) {
    return failure<FileSystemRecreateMutationGuard>(
        clonecore::ErrorCode::internal_error,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成target再識別回数",
        L"target再識別回数が表現上限を超えました");
  }
  ++target_reidentification_count;
  return clonecore::Result<FileSystemRecreateMutationGuard>::success(
      FileSystemRecreateMutationGuard{
          .execution_plan_sha256 = plan.execution_plan_sha256(),
          .source_epoch_sha256 = plan.core_plan().source_epoch_sha256(),
          .target_binding_sha256 = plan.target_binding_sha256(),
      });
}

clonecore::Error cancelled_error() {
  return recreate_error(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      L"再作成target-only transaction",
      L"安全境界で取消が要求されたためtargetをoffline/incompleteのまま停止しました");
}

bool cancellation_requested(
    const clonecore::DiskOperationCallbacks& callbacks) noexcept {
  return clonecore::disk_operation_cancellation_requested(callbacks);
}

bool safe_boundary_cancelled(
    const clonecore::DiskOperationCallbacks& callbacks,
    const std::uint32_t partition_index,
    const std::uint64_t bytes,
    const std::uint64_t units) noexcept {
  return clonecore::disk_operation_control_at_safe_boundary(
             callbacks,
             clonecore::DiskOperationSafeBoundary{
                 .kind = clonecore::DiskOperationSafeBoundaryKind::verified_chunk,
                 .stage = clonecore::DiskOperationStage::copying_data,
                 .partition_index = partition_index,
                 .completed_bytes = bytes,
                 .completed_units = units,
             }) == clonecore::DiskOperationControlDecision::cancel_operation;
}

clonecore::Result<FileSystemRecreateTransactionOutcome>
abort_target_with_error(
    IFileSystemRecreateTargetPlatform& target,
    const FileSystemRecreateMutationGuard& expected_guard,
    const clonecore::Error& original) {
  const auto aborted = target.abort_keep_offline_incomplete(expected_guard);
  if (!guard_equal(aborted.accepted_guard, expected_guard) ||
      !aborted.guard_revalidated_inside_adapter ||
      !aborted.exact_target_handle_retained || !aborted.target_offline ||
      !aborted.completion_incomplete) {
    return clonecore::Result<FileSystemRecreateTransactionOutcome>::success(
        FileSystemRecreateTransactionOutcome{
            .disposition = FileSystemRecreateTransactionDisposition::
                partial_publication_use_prohibited,
            .abort = aborted,
            .error = recreate_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_STATE,
                L"再作成target abort証跡",
                L"失敗後にtargetをofflineかつincompleteへ保持した証跡がありません"),
        });
  }
  return clonecore::Result<FileSystemRecreateTransactionOutcome>::success(
      FileSystemRecreateTransactionOutcome{
          .disposition =
              FileSystemRecreateTransactionDisposition::aborted_incomplete,
          .abort = aborted,
          .error = original,
      });
}

clonecore::Result<FileSystemRecreateTransactionOutcome>
partial_publication_outcome(
    const FileSystemRecreateCompletionEvidence& completion,
    const clonecore::Error& error) {
  return clonecore::Result<FileSystemRecreateTransactionOutcome>::success(
      FileSystemRecreateTransactionOutcome{
          .disposition = FileSystemRecreateTransactionDisposition::
              partial_publication_use_prohibited,
          .completion = completion,
          .error = error,
      });
}

void report_progress(
    const FileSystemRecreateExecutionOptions& options,
    const clonecore::DiskOperationStage stage,
    const std::uint32_t partition_index,
    const std::uint64_t total_bytes,
    const std::uint64_t copied_bytes,
    const bool cancellation_allowed) noexcept {
  clonecore::report_disk_operation_progress(
      options.callbacks,
      clonecore::DiskOperationProgress{
          .stage = stage,
          .partition_index = partition_index,
          .total_read_bytes = total_bytes,
          .total_write_bytes = total_bytes,
          .total_verify_bytes = total_bytes,
          .read_bytes = copied_bytes,
          .written_bytes = copied_bytes,
          .verified_bytes = copied_bytes,
          .cancellation_allowed = cancellation_allowed,
          .pause_allowed = cancellation_allowed,
      });
}

}  // namespace

clonecore::Result<std::unique_ptr<IFileSystemRecreateSourceSession>>
open_windows_file_system_recreate_source_session_read_only(
    const WindowsFileSystemRecreateSourceRequest& request) {
  const auto valid = validate_source_request(request);
  if (!valid) {
    return clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceSession>>::failure(
        valid.error());
  }
  const auto root_kind = classify_source_root(request.source_root_path);
  if (!root_kind.has_value()) {
    return failure<std::unique_ptr<IFileSystemRecreateSourceSession>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"再作成source root",
        L"canonical Volume GUIDまたはVSS snapshot device rootではありません");
  }
  auto root = open_source_root_read_only(request.source_root_path);
  if (!root) {
    return clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceSession>>::failure(
        root.error());
  }
  auto before = observe_source_binding(
      request, root_kind.value(), root.value().get());
  if (!before) {
    return clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceSession>>::failure(
        before.error());
  }

  TreeScanState scan{
      .file_system = request.expected_file_system,
      .limits = request.limits,
  };
  scan.entries.reserve((std::min)(
      request.limits.maximum_entries, std::size_t{4096U}));
  const auto scanned = scan_directory_contents(
      root.value().get(), L"", 0U, scan);
  if (!scanned) {
    return clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceSession>>::failure(
        scanned.error());
  }
  auto after = observe_source_binding(
      request, root_kind.value(), root.value().get());
  if (!after) {
    return clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceSession>>::failure(
        after.error());
  }
  if (!same_source_binding(before.value(), after.value())) {
    return failure<std::unique_ptr<IFileSystemRecreateSourceSession>>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_REVISION_MISMATCH,
        L"再作成source 完全列挙epoch",
        L"完全列挙前後でsource disk、root FileId、extent、filesystem、またはepoch tokenが変化しました");
  }
  std::sort(
      scan.entries.begin(),
      scan.entries.end(),
      [](const ScannedEntry& left, const ScannedEntry& right) {
        return OrdinalCaseInsensitiveLess{}(
            left.canonical.relative_path,
            right.canonical.relative_path);
      });

  migrationcore::CanonicalFileSystemTree tree{
      .file_system = request.expected_file_system,
      .source_table_index = request.source_table_index,
      .enumeration_epoch_sha256 = before.value().enumeration_epoch,
      .namespace_fully_enumerated = true,
      .opened_handles_only = true,
      .every_regular_file_hashed_to_stable_eof = true,
      .short_name_aliases_collision_free = true,
  };
  tree.entries.reserve(scan.entries.size());
  std::vector<ExactHandleMetadata> exact_entries;
  exact_entries.reserve(scan.entries.size());
  for (auto& entry : scan.entries) {
    tree.entries.push_back(std::move(entry.canonical));
    exact_entries.push_back(std::move(entry.exact));
  }
  return clonecore::Result<
      std::unique_ptr<IFileSystemRecreateSourceSession>>::success(
      std::make_unique<WindowsFileSystemRecreateSourceSession>(
          request,
          root_kind.value(),
          root.take_value(),
          before.take_value(),
          std::move(tree),
          std::move(exact_entries)));
}

WindowsFileSystemRecreateExecutionPlan::
    WindowsFileSystemRecreateExecutionPlan(
        migrationcore::FileSystemRecreatePlan core_plan,
        clonecore::StableDiskIdentity source_disk,
        FileSystemRecreateTargetSelection target,
        migrationcore::FileSystemRecreateSha256 target_binding_sha256,
        migrationcore::FileSystemRecreateSha256 execution_plan_sha256)
    : core_plan_(std::move(core_plan)),
      source_disk_(std::move(source_disk)),
      target_(std::move(target)),
      target_binding_sha256_(target_binding_sha256),
      execution_plan_sha256_(execution_plan_sha256) {}

clonecore::Result<WindowsFileSystemRecreateExecutionPlan>
bind_windows_file_system_recreate_execution_plan(
    const migrationcore::FileSystemRecreatePlan& core_plan,
    const IFileSystemRecreateSourceSession& source,
    const FileSystemRecreateTargetSelection& target) {
  const auto source_valid = validate_source_matches_core_plan(
      core_plan, source);
  if (!source_valid) {
    return clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(
        source_valid.error());
  }
  const auto source_identity = clonecore::validate_stable_identity(
      source.source_disk(), source.source_disk(), L"再作成コピー元");
  if (!source_identity) {
    return clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(
        source_identity.error());
  }
  const auto target_identity = clonecore::validate_stable_identity(
      target.expected_target_disk,
      target.expected_target_disk,
      L"再作成コピー先");
  if (!target_identity) {
    return clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(
        target_identity.error());
  }
  if (same_stable_device(source.source_disk(), target.expected_target_disk)) {
    return failure<WindowsFileSystemRecreateExecutionPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"再作成source/target分離",
        L"コピー元とコピー先が同一physical diskを示しています");
  }
  if (target.expected_target_disk.is_system_disk ||
      target.reviewed_as_active_rescue_media ||
      target.target_partition_number !=
          core_plan.target_partition_number() ||
      target.target_partition_offset_bytes !=
          core_plan.target_partition_offset_bytes() ||
      target.target_partition_length_bytes !=
          core_plan.target_geometry().target_volume_bytes ||
      target.target_partition_number == 0U ||
      target.target_partition_offset_bytes == 0U ||
      target.target_partition_length_bytes == 0U ||
      target.target_partition_offset_bytes >
          target.expected_target_disk.size_bytes ||
      target.target_partition_length_bytes >
          target.expected_target_disk.size_bytes -
              target.target_partition_offset_bytes ||
      target.expected_target_disk.logical_sector_size !=
          core_plan.target_geometry().logical_sector_size ||
      target.target_partition_offset_bytes %
              target.expected_target_disk.logical_sector_size !=
          0U ||
      target.target_partition_length_bytes %
              target.expected_target_disk.logical_sector_size !=
          0U) {
    return failure<WindowsFileSystemRecreateExecutionPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成target execution binding",
        L"system/rescue保護、exact partition extent、またはtarget sector geometryがpure core planと一致しません");
  }
  auto target_binding = hash_target_binding(
      target.expected_target_disk,
      target.target_partition_number,
      target.target_partition_offset_bytes,
      target.target_partition_length_bytes);
  if (!target_binding) {
    return clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(
        target_binding.error());
  }
  auto execution_hash = hash_execution_plan(
      core_plan, source.source_disk(), target_binding.value());
  if (!execution_hash) {
    return clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(
        execution_hash.error());
  }
  return clonecore::Result<WindowsFileSystemRecreateExecutionPlan>::success(
      WindowsFileSystemRecreateExecutionPlan(
          core_plan,
          source.source_disk(),
          target,
          target_binding.value(),
          execution_hash.value()));
}

namespace {

bool production_guard_equal(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const FileSystemRecreateMutationGuard& guard) noexcept {
  return guard.execution_plan_sha256 == plan.execution_plan_sha256() &&
      guard.source_epoch_sha256 == plan.core_plan().source_epoch_sha256() &&
      guard.target_binding_sha256 == plan.target_binding_sha256();
}

bool production_volume_equal(
    const WindowsFileSystemRecreateConstructionVolumeBinding& left,
    const WindowsFileSystemRecreateConstructionVolumeBinding& right) noexcept {
  return left.final_target_number == right.final_target_number &&
      left.disk_number == right.disk_number &&
      left.target_offset == right.target_offset &&
      left.target_size == right.target_size &&
      same_text_case_insensitive(
          normalized_root_path(left.canonical_volume_guid_path),
          normalized_root_path(right.canonical_volume_guid_path)) &&
      left.exact_single_disk_extent && right.exact_single_disk_extent;
}

bool valid_production_request(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const WindowsFileSystemRecreateProductionTargetRequest& request) noexcept {
  const auto& core = plan.core_plan();
  const auto& target = plan.target();
  const auto& final = request.reviewed_layout;
  if (!request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token !=
          clonecore::make_target_confirmation_token(
              target.expected_target_disk) ||
      is_zero_digest(request.expected_original_target_layout_sha256) ||
      request.target_is_active_rescue_media ||
      target.reviewed_as_active_rescue_media ||
      target.expected_target_disk.is_system_disk ||
      // The current DiskModel physical-target gate intentionally accepts only
      // 512-byte logical targets.  Do not claim a connected 4Kn target path
      // until that independently reviewed gate changes.
      target.expected_target_disk.logical_sector_size != 512U ||
      final.metadata.target_size_bytes !=
          target.expected_target_disk.size_bytes ||
      final.metadata.logical_sector_size !=
          target.expected_target_disk.logical_sector_size ||
      final.migration.target_size_bytes !=
          target.expected_target_disk.size_bytes ||
      !final.migration.source_remains_unchanged ||
      final.migration.target_partitions.size() != 1U ||
      request.reviewed_construction_layouts.size() != 1U ||
      (final.metadata.staged_writes.empty() &&
       final.metadata.commit_writes.empty())) {
    return false;
  }

  const auto& partition = final.migration.target_partitions.front();
  const auto& construction =
      request.reviewed_construction_layouts.front();
  const bool file_system_action =
      partition.action ==
          migrationcore::MigrationPartitionAction::apply_file_image ||
      (core.target_geometry().file_system ==
           migrationcore::MigrationFileSystem::fat32 &&
       partition.action ==
           migrationcore::MigrationPartitionAction::create_empty_fat32) ||
      (core.target_geometry().file_system ==
           migrationcore::MigrationFileSystem::exfat &&
       partition.action ==
           migrationcore::MigrationPartitionAction::create_empty_exfat);
  const auto expected_purpose = partition.action ==
          migrationcore::MigrationPartitionAction::apply_file_image
      ? imageformat::TsumugiShrinkConstructionPurposeV1::apply_file_image
      : imageformat::TsumugiShrinkConstructionPurposeV1::
            recreate_empty_file_system;
  return is_supported_recreate_file_system(partition.file_system) &&
      partition.file_system == core.target_geometry().file_system &&
      file_system_action &&
      partition.target_number == 1U &&
      partition.target_number == target.target_partition_number &&
      partition.target_number == core.target_partition_number() &&
      partition.offset_bytes == target.target_partition_offset_bytes &&
      partition.offset_bytes == core.target_partition_offset_bytes() &&
      partition.size_bytes == target.target_partition_length_bytes &&
      partition.size_bytes == core.target_geometry().target_volume_bytes &&
      partition.source_table_index.has_value() &&
      partition.source_table_index.value() == core.source_table_index() &&
      construction.purpose == expected_purpose &&
      construction.source_table_index == partition.source_table_index &&
      construction.final_target_number == partition.target_number &&
      construction.target_offset == partition.offset_bytes &&
      construction.target_size == partition.size_bytes &&
      construction.temporary_metadata.target_size_bytes ==
          final.metadata.target_size_bytes &&
      construction.temporary_metadata.logical_sector_size ==
          final.metadata.logical_sector_size &&
      !construction.retirement_ranges.empty();
}

class WindowsFileSystemRecreateProductionTargetPlatform final
    : public IWindowsFileSystemRecreateProductionTargetPlatform {
 public:
  WindowsFileSystemRecreateProductionTargetPlatform(
      const WindowsFileSystemRecreateExecutionPlan& plan,
      WindowsFileSystemRecreateProductionTargetRequest request,
      std::unique_ptr<IWindowsFileSystemRecreateTargetIo> io)
      : plan_(plan), request_(std::move(request)), io_(std::move(io)) {}

  [[nodiscard]] WindowsFileSystemRecreateTargetLifecycleState
  lifecycle_state() const noexcept override {
    return state_;
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateTargetObservation>
  reidentify_target_read_only() override {
    return observe_state(state_ ==
            WindowsFileSystemRecreateTargetLifecycleState::
                construction_volume_online ||
        state_ == WindowsFileSystemRecreateTargetLifecycleState::
                namespace_readback_verified);
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateMutationEvidence>
  begin_incomplete_target(
      const FileSystemRecreateMutationGuard& guard) override {
    if (state_ != WindowsFileSystemRecreateTargetLifecycleState::ready ||
        !production_guard_equal(plan_, guard)) {
      return invalid_state_result<FileSystemRecreateMutationEvidence>(
          L"再作成production target開始guard");
    }
    auto observed = observe_state(false, true);
    if (!observed) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          observed.error());
    }
    auto offline = io_->set_target_offline(true);
    if (!offline) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          offline.error());
    }
    auto opened = io_->open_offline_target();
    if (!opened) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          opened.error());
    }
    const auto identity = clonecore::validate_stable_identity(
        plan_.target().expected_target_disk,
        opened.value().observed.target_identity,
        L"再作成production opened target");
    if (!identity || !opened.value().target ||
        opened.value().target->size_bytes() !=
            plan_.target().expected_target_disk.size_bytes ||
        opened.value().target->logical_sector_size() !=
            plan_.target().expected_target_disk.logical_sector_size) {
      return !identity
          ? clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
                identity.error())
          : invalid_state_result<FileSystemRecreateMutationEvidence>(
                L"再作成production opened target寸法");
    }
    target_ = std::move(opened.take_value());
    auto locked = observe_state(false, true);
    if (!locked) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          locked.error());
    }
    try {
      layout_ = std::make_unique<
          imageformat::TsumugiShrinkRestoreLayoutTransactionV1>(
          request_.reviewed_layout,
          request_.reviewed_construction_layouts,
          *target_.target);
    } catch (...) {
      return internal_exception_result<FileSystemRecreateMutationEvidence>(
          L"再作成production layout transaction生成");
    }
    auto prepared = layout_->prepare(request_.callbacks);
    state_ = WindowsFileSystemRecreateTargetLifecycleState::
        offline_incomplete;
    if (!prepared) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          prepared.error());
    }
    return clonecore::Result<FileSystemRecreateMutationEvidence>::success(
        mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::physical_disk_offline,
            false));
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateMutationEvidence>
  format_target_file_system(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::FileSystemRecreateFormatGeometry& desired)
      override {
    if (state_ != WindowsFileSystemRecreateTargetLifecycleState::
            offline_incomplete ||
        !production_guard_equal(plan_, guard) ||
        !geometry_equal(desired, plan_.core_plan().target_geometry()) ||
        !target_.target || !layout_) {
      return invalid_state_result<FileSystemRecreateMutationEvidence>(
          L"再作成production format guard");
    }
    auto observed = observe_state(false);
    if (!observed) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          observed.error());
    }
    auto published = layout_->publish_construction(
        plan_.target().target_partition_number, request_.callbacks);
    if (!published) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          published.error());
    }
    auto status = io_->notify_layout_changed();
    if (!status) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          status.error());
    }
    status = io_->set_target_offline(false);
    if (!status) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          status.error());
    }
    state_ = WindowsFileSystemRecreateTargetLifecycleState::
        construction_volume_online;
    auto volume = io_->bind_online_construction_volume(
        plan_.target().target_partition_number,
        plan_.target().target_partition_offset_bytes,
        plan_.target().target_partition_length_bytes);
    if (!volume) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          volume.error());
    }
    binding_ = volume.take_value();
    status = io_->format_exact_volume(*binding_, desired);
    if (!status) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          status.error());
    }
    auto root = io_->open_and_inspect_formatted_root(*binding_, desired);
    if (!root) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          root.error());
    }
    const auto root_valid = validate_root(root.value());
    if (!root_valid) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          root_valid.error());
    }
    root_ = root.take_value();
    return clonecore::Result<FileSystemRecreateMutationEvidence>::success(
        mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            false));
  }

  [[nodiscard]] clonecore::Result<
      FileSystemRecreateFormattedTargetObservation>
  inspect_formatted_target_read_only() override {
    auto target = observe_state(true);
    if (!target) {
      return clonecore::Result<
          FileSystemRecreateFormattedTargetObservation>::failure(
          target.error());
    }
    auto root = io_->revalidate_formatted_root_read_only();
    if (!root) {
      return clonecore::Result<
          FileSystemRecreateFormattedTargetObservation>::failure(
          root.error());
    }
    const auto root_valid = validate_root(root.value());
    if (!root_valid) {
      return clonecore::Result<
          FileSystemRecreateFormattedTargetObservation>::failure(
          root_valid.error());
    }
    root_ = root.take_value();
    return clonecore::Result<
        FileSystemRecreateFormattedTargetObservation>::success({
        .target = target.take_value(),
        .actual_geometry = root_->actual_geometry,
        .root_reparse_tag = root_->root_reparse_tag,
        .root_is_directory = root_->root_is_directory,
        .root_opened_handle_identity_stable =
            root_->root_opened_handle_identity_stable,
    });
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateMutationEvidence>
  create_directory_no_replace(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto boundary = validate_online_mutation_guard(guard);
    if (!boundary) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          boundary.error());
    }
    auto created = io_->create_directory_no_replace(entry);
    if (!created) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          created.error());
    }
    return clonecore::Result<FileSystemRecreateMutationEvidence>::success(
        mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            false));
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateTargetFile>
  create_file_no_replace(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto boundary = validate_online_mutation_guard(guard);
    if (!boundary) {
      return clonecore::Result<FileSystemRecreateTargetFile>::failure(
          boundary.error());
    }
    auto created = io_->create_file_no_replace(entry);
    if (!created || created.value() == 0U) {
      return created
          ? invalid_state_result<FileSystemRecreateTargetFile>(
                L"再作成production file token")
          : clonecore::Result<FileSystemRecreateTargetFile>::failure(
                created.error());
    }
    return clonecore::Result<FileSystemRecreateTargetFile>::success({
        .opened_handle_token = created.value(),
        .mutation = mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            false),
    });
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateTargetWrite>
  write_file_chunk(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetFile& file,
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    auto boundary = validate_online_mutation_guard(guard);
    if (!boundary || file.opened_handle_token == 0U || bytes.empty()) {
      return !boundary
          ? clonecore::Result<FileSystemRecreateTargetWrite>::failure(
                boundary.error())
          : invalid_state_result<FileSystemRecreateTargetWrite>(
                L"再作成production file write引数");
    }
    auto written = io_->write_file_chunk(
        file.opened_handle_token, offset, bytes);
    if (!written) {
      return clonecore::Result<FileSystemRecreateTargetWrite>::failure(
          written.error());
    }
    return clonecore::Result<FileSystemRecreateTargetWrite>::success({
        .bytes_written = written.value(),
        .mutation = mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            false),
    });
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateMutationEvidence>
  finalize_file_metadata_flush_and_close(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetFile& file,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto boundary = validate_online_mutation_guard(guard);
    if (!boundary || file.opened_handle_token == 0U) {
      return !boundary
          ? clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
                boundary.error())
          : invalid_state_result<FileSystemRecreateMutationEvidence>(
                L"再作成production file finalize token");
    }
    auto finalized = io_->finalize_file_metadata_flush_and_hold(
        file.opened_handle_token, entry);
    if (!finalized) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          finalized.error());
    }
    return clonecore::Result<FileSystemRecreateMutationEvidence>::success(
        mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            true));
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateMutationEvidence>
  apply_directory_metadata_and_flush(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) override {
    auto boundary = validate_online_mutation_guard(guard);
    if (!boundary) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          boundary.error());
    }
    auto applied = io_->apply_directory_metadata_and_flush(entry);
    if (!applied) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          applied.error());
    }
    return clonecore::Result<FileSystemRecreateMutationEvidence>::success(
        mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            true));
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateMutationEvidence>
  flush_target_namespace(
      const FileSystemRecreateMutationGuard& guard) override {
    auto boundary = validate_online_mutation_guard(guard);
    if (!boundary) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          boundary.error());
    }
    auto flushed = io_->flush_target_namespace();
    if (!flushed) {
      return clonecore::Result<FileSystemRecreateMutationEvidence>::failure(
          flushed.error());
    }
    return clonecore::Result<FileSystemRecreateMutationEvidence>::success(
        mutation_evidence(
            guard,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            true));
  }

  [[nodiscard]] clonecore::Result<
      migrationcore::FileSystemRecreateTargetReadback>
  enumerate_complete_target_readback_read_only(
      const migrationcore::FileSystemRecreatePlan& plan) override {
    if (state_ != WindowsFileSystemRecreateTargetLifecycleState::
            construction_volume_online ||
        plan.plan_sha256() != plan_.core_plan().plan_sha256()) {
      return invalid_state_result<
          migrationcore::FileSystemRecreateTargetReadback>(
          L"再作成production full readback状態");
    }
    auto observed = observe_state(true);
    if (!observed) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateTargetReadback>::failure(
          observed.error());
    }
    auto readback =
        io_->enumerate_complete_target_readback_read_only(plan);
    if (!readback) {
      return readback;
    }
    auto verified = migrationcore::verify_recreated_file_system_tree(
        plan, readback.value());
    if (!verified) {
      return clonecore::Result<
          migrationcore::FileSystemRecreateTargetReadback>::failure(
          verified.error());
    }
    verified_readback_ = verified.take_value();
    state_ = WindowsFileSystemRecreateTargetLifecycleState::
        namespace_readback_verified;
    return readback;
  }

  [[nodiscard]] FileSystemRecreateCommitOutcome commit_completion_last(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::FileSystemRecreateVerification& verification)
      noexcept override {
    FileSystemRecreateCompletionEvidence evidence{
        .accepted_guard = guard,
        .verified_manifest_sha256 = verification.observed_manifest_sha256,
        .target_epoch_sha256 = verification.target_epoch_sha256,
        .guard_revalidated_inside_adapter =
            production_guard_equal(plan_, guard),
        .complete_readback_verified = verified_readback_.has_value(),
        .target_offline = false,
        .incomplete_use_prohibited = true,
    };
    try {
      if (state_ != WindowsFileSystemRecreateTargetLifecycleState::
              namespace_readback_verified ||
          !production_guard_equal(plan_, guard) || !verified_readback_ ||
          verification.observed_manifest_sha256 !=
              verified_readback_->observed_manifest_sha256 ||
          verification.target_epoch_sha256 !=
              verified_readback_->target_epoch_sha256 ||
          !binding_ || !layout_ || !target_.target) {
        return prepublication_failure(
            std::move(evidence),
            clonecore::ErrorCode::verification_failed,
            ERROR_REVISION_MISMATCH);
      }
      auto online = observe_state(true);
      if (!online) {
        return prepublication_failure_from_error(
            std::move(evidence), online.error());
      }
      auto status = io_->close_namespace_dismount_and_offline(*binding_);
      if (!status) {
        return prepublication_failure_from_error(
            std::move(evidence), status.error());
      }
      state_ = WindowsFileSystemRecreateTargetLifecycleState::
          offline_incomplete;
      auto offline = observe_state(false);
      if (!offline) {
        return prepublication_failure_from_error(
            std::move(evidence), offline.error());
      }
      evidence.target_offline = true;
      auto retired = layout_->retire_construction(
          plan_.target().target_partition_number, request_.callbacks);
      if (!retired) {
        return prepublication_failure_from_error(
            std::move(evidence), retired.error());
      }
      status = io_->notify_layout_changed();
      if (!status) {
        return prepublication_failure_from_error(
            std::move(evidence), status.error());
      }
      offline = observe_state(false);
      if (!offline) {
        return prepublication_failure_from_error(
            std::move(evidence), offline.error());
      }
      state_ = WindowsFileSystemRecreateTargetLifecycleState::
          construction_retired_offline;
      offline_proven_before_publication_ = true;
      if (clonecore::disk_operation_cancellation_requested(
              request_.callbacks)) {
        return prepublication_failure(
            std::move(evidence),
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED);
      }

      const auto& metadata = request_.reviewed_layout.metadata;
      if (metadata.staged_writes.empty() &&
          metadata.commit_writes.empty() ||
          !offline_proven_before_publication_) {
        return prepublication_failure(
            std::move(evidence),
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA);
      }
      state_ = WindowsFileSystemRecreateTargetLifecycleState::
          publication_attempted;
      evidence.publication_attempted = true;
      const auto publish_write = [&](
          const imageformat::TsumugiRestoreLayoutWrite& write) {
        auto written = target_.target->write_target(write.offset, write.bytes);
        if (!written) {
          return written;
        }
        written = target_.target->flush_target();
        if (!written) {
          return written;
        }
        auto readback = target_.target->read_back(
            write.offset, write.bytes.size());
        if (!readback || readback.value() != write.bytes) {
          return readback
              ? status_failure(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_CRC,
                    L"再作成production final metadata読戻し",
                    L"最終partition metadataが同じphysical handleで一致しません")
              : clonecore::Status::failure(readback.error());
        }
        return clonecore::success_status();
      };
      for (const auto& write : metadata.staged_writes) {
        status = publish_write(write);
        if (!status) {
          return partial_publication(
              std::move(evidence), status.error());
        }
      }
      for (const auto& write : metadata.commit_writes) {
        status = publish_write(write);
        if (!status) {
          return partial_publication(
              std::move(evidence), status.error());
        }
      }
      auto inspected =
          imageformat::inspect_tsumugi_whole_disk_restore_layout_publication_v1(
              metadata, *target_.target);
      if (!inspected ||
          inspected.value().state != imageformat::
              TsumugiRestoreLayoutPublicationStateV1::all_final ||
          inspected.value().published_write_count !=
              inspected.value().total_write_count) {
        return partial_publication(
            std::move(evidence),
            inspected
                ? recreate_error(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_CRC,
                      L"再作成production final layout分類",
                      L"最終partition table全体をall-finalと分類できません")
                : inspected.error());
      }
      evidence.publication_latched = true;
      evidence.publication_readback_verified = true;
      offline = observe_state(false);
      if (!offline) {
        return partial_publication(
            std::move(evidence), offline.error());
      }
      evidence.exact_target_reidentified = true;
      evidence.completion_committed_last = true;
      evidence.target_offline = true;
      evidence.cleanup_pending = false;
      evidence.incomplete_use_prohibited = false;
      state_ = WindowsFileSystemRecreateTargetLifecycleState::
          completed_offline;
      target_.target.reset();
      return FileSystemRecreateCommitOutcome{
          .disposition = FileSystemRecreateCommitDisposition::completed,
          .evidence = evidence,
          .failure_code = clonecore::ErrorCode::verification_failed,
          .native_failure_code = ERROR_SUCCESS,
      };
    } catch (...) {
      if (evidence.publication_attempted ||
          state_ == WindowsFileSystemRecreateTargetLifecycleState::
              publication_attempted) {
        return partial_publication(
            std::move(evidence),
            recreate_error(
                clonecore::ErrorCode::internal_error,
                ERROR_UNHANDLED_EXCEPTION,
                L"再作成production final publication例外",
                L"公開開始後の例外のため破壊的rollbackを行いません"));
      }
      return prepublication_failure(
          std::move(evidence),
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION);
    }
  }

  [[nodiscard]] FileSystemRecreateAbortEvidence
  abort_keep_offline_incomplete(
      const FileSystemRecreateMutationGuard& guard) noexcept override {
    FileSystemRecreateAbortEvidence evidence{
        .accepted_guard = guard,
        .guard_revalidated_inside_adapter =
            production_guard_equal(plan_, guard),
        .exact_target_handle_retained = target_.target != nullptr,
        .target_offline = false,
        .completion_incomplete = true,
    };
    if (!evidence.guard_revalidated_inside_adapter ||
        state_ == WindowsFileSystemRecreateTargetLifecycleState::
            publication_attempted ||
        state_ == WindowsFileSystemRecreateTargetLifecycleState::
            completed_offline ||
        state_ == WindowsFileSystemRecreateTargetLifecycleState::
            partial_publication_use_prohibited) {
      return evidence;
    }
    try {
      clonecore::Status status = clonecore::success_status();
      if (state_ == WindowsFileSystemRecreateTargetLifecycleState::
              construction_volume_online ||
          state_ == WindowsFileSystemRecreateTargetLifecycleState::
              namespace_readback_verified) {
        status = binding_
            ? io_->close_namespace_dismount_and_offline(*binding_)
            : io_->set_target_offline(true);
      } else {
        status = io_->set_target_offline(true);
      }
      if (!status) {
        return evidence;
      }
      state_ = WindowsFileSystemRecreateTargetLifecycleState::
          offline_incomplete;
      if (layout_) {
        layout_->abort();
        status = io_->notify_layout_changed();
        if (!status) {
          return evidence;
        }
      }
      status = io_->set_target_offline(true);
      if (!status) {
        return evidence;
      }
      auto offline = observe_state(false);
      if (!offline) {
        return evidence;
      }
      evidence.target_offline = true;
      evidence.exact_target_handle_retained = target_.target != nullptr;
      state_ = WindowsFileSystemRecreateTargetLifecycleState::
          aborted_offline_incomplete;
      return evidence;
    } catch (...) {
      return evidence;
    }
  }

 private:
  template <typename T>
  [[nodiscard]] clonecore::Result<T> invalid_state_result(
      const std::wstring_view operation) const {
    return failure<T>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        std::wstring(operation),
        L"typed lifecycle、immutable guard、またはreviewed target bindingが一致しません");
  }

  template <typename T>
  [[nodiscard]] clonecore::Result<T> internal_exception_result(
      const std::wstring_view operation) const {
    return failure<T>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        std::wstring(operation),
        L"production target adapter内で例外が発生しました");
  }

  [[nodiscard]] FileSystemRecreateMutationEvidence mutation_evidence(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetIsolationState isolation,
      const bool flushed) const noexcept {
    return FileSystemRecreateMutationEvidence{
        .accepted_guard = guard,
        .guard_revalidated_inside_adapter =
            production_guard_equal(plan_, guard),
        .exact_target_handle_retained = target_.target != nullptr,
        .isolation_state = isolation,
        .completion_incomplete = true,
        .flushed = flushed,
    };
  }

  [[nodiscard]] clonecore::Status validate_disk_observation(
      const WindowsFileSystemRecreateTargetDiskObservation& observed,
      const bool expected_online,
      const bool require_original_layout) const {
    const auto identity = clonecore::validate_stable_identity(
        plan_.target().expected_target_disk,
        observed.physical.target_identity,
        L"再作成production target fresh reidentify");
    if (!identity) {
      return identity;
    }
    const auto& disk = observed.physical.target;
    if (!observed.target_class_accepted || disk.is_system_disk ||
        !disk.offline.has_value() || !disk.read_only.has_value() ||
        disk.read_only.value() || !disk.removable.has_value() ||
        disk.removable.value() || disk.offline.value() == expected_online ||
        disk.size_bytes != plan_.target().expected_target_disk.size_bytes ||
        disk.logical_sector_size !=
            plan_.target().expected_target_disk.logical_sector_size ||
        (require_original_layout &&
         observed.current_layout_sha256 !=
             request_.expected_original_target_layout_sha256)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"再作成production target fresh属性",
          L"stable identity、class、offline/read-only/removable、sector、容量、またはoriginal layout hashが一致しません");
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status validate_root(
      const WindowsFileSystemRecreateFormattedRootObservation& root) const {
    if (!binding_ || !production_volume_equal(root.volume, *binding_) ||
        !geometry_equal(
            root.actual_geometry, plan_.core_plan().target_geometry()) ||
        root.root_reparse_tag != 0U || !root.root_is_directory ||
        !root.root_opened_handle_identity_stable) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"再作成production formatted root binding",
          L"Volume GUID、single extent、actual geometry、FileId、またはnon-reparse rootが固定計画と一致しません");
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateTargetObservation>
  observe_state(
      const bool expected_online,
      const bool require_original_layout = false) {
    auto observed = io_->observe_target_read_only();
    if (!observed) {
      return clonecore::Result<FileSystemRecreateTargetObservation>::failure(
          observed.error());
    }
    const auto valid = validate_disk_observation(
        observed.value(), expected_online, require_original_layout);
    if (!valid) {
      return clonecore::Result<FileSystemRecreateTargetObservation>::failure(
          valid.error());
    }
    bool exact_extent = false;
    bool root_stable = false;
    bool root_non_reparse = false;
    if (expected_online) {
      if (!binding_ || !root_) {
        return invalid_state_result<FileSystemRecreateTargetObservation>(
            L"再作成production online root state");
      }
      auto root = io_->revalidate_formatted_root_read_only();
      if (!root) {
        return clonecore::Result<FileSystemRecreateTargetObservation>::failure(
            root.error());
      }
      const auto root_valid = validate_root(root.value());
      if (!root_valid) {
        return clonecore::Result<FileSystemRecreateTargetObservation>::failure(
            root_valid.error());
      }
      root_ = root.take_value();
      exact_extent = true;
      root_stable = true;
      root_non_reparse = true;
    }
    if (freshness_sequence_ ==
        (std::numeric_limits<std::uint64_t>::max)()) {
      return invalid_state_result<FileSystemRecreateTargetObservation>(
          L"再作成production freshness sequence");
    }
    ++freshness_sequence_;
    return clonecore::Result<FileSystemRecreateTargetObservation>::success({
        .observed_target_disk = observed.value().physical.target_identity,
        .target_partition_number = plan_.target().target_partition_number,
        .target_partition_offset_bytes =
            plan_.target().target_partition_offset_bytes,
        .target_partition_length_bytes =
            plan_.target().target_partition_length_bytes,
        .freshness_sequence = freshness_sequence_,
        .freshly_reidentified = true,
        .reviewed_extent_within_disk = true,
        .exact_partition_extent = exact_extent,
        .isolation_state = expected_online
            ? FileSystemRecreateTargetIsolationState::
                  construction_volume_online_exclusive
            : FileSystemRecreateTargetIsolationState::physical_disk_offline,
        .exact_target_handle_retained = target_.target != nullptr,
        .root_file_id_stable = root_stable,
        .root_is_non_reparse = root_non_reparse,
        .active_rescue_media = request_.target_is_active_rescue_media,
    });
  }

  [[nodiscard]] clonecore::Status validate_online_mutation_guard(
      const FileSystemRecreateMutationGuard& guard) {
    if ((state_ != WindowsFileSystemRecreateTargetLifecycleState::
             construction_volume_online &&
         state_ != WindowsFileSystemRecreateTargetLifecycleState::
             namespace_readback_verified) ||
        !production_guard_equal(plan_, guard) || !target_.target ||
        !binding_ || !root_) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"再作成production online mutation guard",
          L"typed state、immutable guard、target handle、またはVolume root bindingが一致しません");
    }
    auto observed = observe_state(true);
    return observed
        ? clonecore::success_status()
        : clonecore::Status::failure(observed.error());
  }

  [[nodiscard]] static FileSystemRecreateCommitOutcome
  prepublication_failure(
      FileSystemRecreateCompletionEvidence evidence,
      const clonecore::ErrorCode code,
      const DWORD native) noexcept {
    evidence.publication_attempted = false;
    evidence.publication_latched = false;
    evidence.publication_readback_verified = false;
    evidence.completion_committed_last = false;
    evidence.incomplete_use_prohibited = true;
    return FileSystemRecreateCommitOutcome{
        .disposition = FileSystemRecreateCommitDisposition::
            prepublication_failure,
        .evidence = evidence,
        .failure_code = code,
        .native_failure_code = native,
    };
  }

  [[nodiscard]] static FileSystemRecreateCommitOutcome
  prepublication_failure_from_error(
      FileSystemRecreateCompletionEvidence evidence,
      const clonecore::Error& error) noexcept {
    return prepublication_failure(
        std::move(evidence), error.code, error.native_code);
  }

  [[nodiscard]] FileSystemRecreateCommitOutcome partial_publication(
      FileSystemRecreateCompletionEvidence evidence,
      const clonecore::Error& error) noexcept {
    evidence.publication_attempted = true;
    evidence.completion_committed_last = false;
    // The proof immediately before publication is a prerequisite, but it is
    // not a truthful post-failure proof.  Refresh read-only while the exact
    // physical handle is still retained; an unreadable/torn table therefore
    // reports target_offline=false instead of borrowing stale evidence.
    evidence.target_offline = false;
    try {
      const auto fresh_offline = observe_state(false);
      evidence.target_offline = fresh_offline.has_value();
    } catch (...) {
      evidence.target_offline = false;
    }
    evidence.incomplete_use_prohibited = true;
    state_ = WindowsFileSystemRecreateTargetLifecycleState::
        partial_publication_use_prohibited;
    target_.target.reset();
    return FileSystemRecreateCommitOutcome{
        .disposition = FileSystemRecreateCommitDisposition::
            partial_publication_use_prohibited,
        .evidence = evidence,
        .failure_code = error.code,
        .native_failure_code = error.native_code,
    };
  }

  const WindowsFileSystemRecreateExecutionPlan plan_;
  const WindowsFileSystemRecreateProductionTargetRequest request_;
  std::unique_ptr<IWindowsFileSystemRecreateTargetIo> io_;
  diskmodel::PhysicalTargetHandle target_;
  std::unique_ptr<imageformat::TsumugiShrinkRestoreLayoutTransactionV1>
      layout_;
  std::optional<WindowsFileSystemRecreateConstructionVolumeBinding> binding_;
  std::optional<WindowsFileSystemRecreateFormattedRootObservation> root_;
  std::optional<migrationcore::FileSystemRecreateVerification>
      verified_readback_;
  WindowsFileSystemRecreateTargetLifecycleState state_{
      WindowsFileSystemRecreateTargetLifecycleState::ready};
  std::uint64_t freshness_sequence_{};
  bool offline_proven_before_publication_{};
};

}  // namespace

clonecore::Result<std::unique_ptr<
    IWindowsFileSystemRecreateProductionTargetPlatform>>
make_windows_file_system_recreate_target_platform_with_io(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const WindowsFileSystemRecreateProductionTargetRequest& request,
    std::unique_ptr<IWindowsFileSystemRecreateTargetIo> io) {
  if (!io || !valid_production_request(plan, request)) {
    return failure<std::unique_ptr<
        IWindowsFileSystemRecreateProductionTargetPlatform>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成production target factory",
        L"完全な単一区画FAT32/exFAT reviewed layout、construction plan、target confirmation、または512-byte target gateが一致しません");
  }
  try {
    return clonecore::Result<std::unique_ptr<
        IWindowsFileSystemRecreateProductionTargetPlatform>>::success(
        std::make_unique<
            WindowsFileSystemRecreateProductionTargetPlatform>(
            plan, request, std::move(io)));
  } catch (...) {
    return failure<std::unique_ptr<
        IWindowsFileSystemRecreateProductionTargetPlatform>>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"再作成production target factory allocation",
        L"production target adapterを生成できませんでした");
  }
}

clonecore::Result<FileSystemRecreateTransactionOutcome>
execute_file_system_recreation_on_injected_target(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    IFileSystemRecreateSourceSession& source,
    IFileSystemRecreateTargetPlatform& target,
    const FileSystemRecreateExecutionOptions& options) {
  if (options.maximum_transfer_bytes == 0U ||
      options.maximum_transfer_bytes >
          kDefaultWindowsFileSystemRecreateTransferBytes) {
    return failure<FileSystemRecreateTransactionOutcome>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"再作成transfer chunk上限",
        L"transfer chunkは1..1MiBの固定上限内で指定する必要があります");
  }
  const auto source_bound = validate_source_matches_core_plan(
      plan.core_plan(), source);
  if (!source_bound) {
    return clonecore::Result<FileSystemRecreateTransactionOutcome>::failure(
        source_bound.error());
  }

  std::uint64_t source_sequence{};
  std::uint64_t target_sequence{};
  std::uint64_t target_reidentification_count{};
  std::uint64_t copied_file_bytes{};
  std::uint64_t completed_units{};
  std::uint64_t directory_count{};
  std::uint64_t regular_file_count{};
  bool mutation_started{};
  bool publication_may_have_started{};
  FileSystemRecreateCompletionEvidence last_completion_evidence{};
  const FileSystemRecreateMutationGuard transaction_guard{
      .execution_plan_sha256 = plan.execution_plan_sha256(),
      .source_epoch_sha256 = plan.core_plan().source_epoch_sha256(),
      .target_binding_sha256 = plan.target_binding_sha256(),
  };
  const auto abort_with_error =
      [&](IFileSystemRecreateTargetPlatform& failed_target,
          const clonecore::Error& error) {
        return abort_target_with_error(
            failed_target, transaction_guard, error);
      };

  report_progress(
      options,
      clonecore::DiskOperationStage::planning,
      plan.core_plan().target_partition_number(),
      plan.core_plan().capacity().total_content_bytes,
      0U,
      true);
  if (cancellation_requested(options.callbacks)) {
    return clonecore::Result<FileSystemRecreateTransactionOutcome>::failure(
        cancelled_error());
  }

  try {
    auto begin_guard = revalidate_mutation_boundary(
        plan,
        source,
        target,
        source_sequence,
        target_sequence,
        target_reidentification_count,
        FileSystemRecreateTargetIsolationState::physical_disk_offline,
        false);
    if (!begin_guard) {
      return clonecore::Result<FileSystemRecreateTransactionOutcome>::failure(
          begin_guard.error());
    }
    if (cancellation_requested(options.callbacks)) {
      return clonecore::Result<FileSystemRecreateTransactionOutcome>::failure(
          cancelled_error());
    }
    mutation_started = true;
    auto begun = target.begin_incomplete_target(begin_guard.value());
    if (!begun) {
      return abort_with_error(target, begun.error());
    }
    const auto begun_valid = validate_mutation_evidence(
        begun.value(),
        begin_guard.value(),
        FileSystemRecreateTargetIsolationState::physical_disk_offline,
        false);
    if (!begun_valid) {
      return abort_with_error(target, begun_valid.error());
    }

    report_progress(
        options,
        clonecore::DiskOperationStage::invalidating_target,
        plan.core_plan().target_partition_number(),
        plan.core_plan().capacity().total_content_bytes,
        copied_file_bytes,
        false);
    auto format_guard = revalidate_mutation_boundary(
        plan,
        source,
        target,
        source_sequence,
        target_sequence,
        target_reidentification_count,
        FileSystemRecreateTargetIsolationState::physical_disk_offline,
        true);
    if (!format_guard) {
      return abort_with_error(target, format_guard.error());
    }
    if (cancellation_requested(options.callbacks)) {
      return abort_with_error(target, cancelled_error());
    }
    auto formatted = target.format_target_file_system(
        format_guard.value(), plan.core_plan().target_geometry());
    if (!formatted) {
      return abort_with_error(target, formatted.error());
    }
    const auto format_valid = validate_mutation_evidence(
        formatted.value(),
        format_guard.value(),
        FileSystemRecreateTargetIsolationState::
            construction_volume_online_exclusive,
        false);
    if (!format_valid) {
      return abort_with_error(target, format_valid.error());
    }

    auto actual_format = target.inspect_formatted_target_read_only();
    if (!actual_format) {
      return abort_with_error(target, actual_format.error());
    }
    const auto actual_target_valid = validate_target_observation(
        plan,
        actual_format.value().target,
        target_sequence,
        FileSystemRecreateTargetIsolationState::
            construction_volume_online_exclusive,
        true);
    if (!actual_target_valid) {
      return abort_with_error(target, actual_target_valid.error());
    }
    target_sequence = actual_format.value().target.freshness_sequence;
    ++target_reidentification_count;
    if (!geometry_equal(
            actual_format.value().actual_geometry,
            plan.core_plan().target_geometry()) ||
        actual_format.value().root_reparse_tag != 0U ||
        !actual_format.value().root_is_directory ||
        !actual_format.value().root_opened_handle_identity_stable) {
      return abort_with_error(
          target,
          recreate_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"再作成format actual geometry/root読戻し",
              L"formatter後のactual geometryまたは非reparse root handle証跡が計画と一致しません"));
    }

    report_progress(
        options,
        clonecore::DiskOperationStage::copying_data,
        plan.core_plan().target_partition_number(),
        plan.core_plan().capacity().total_content_bytes,
        copied_file_bytes,
        true);
    const auto entries = plan.core_plan().entries();
    for (std::size_t index = 0U; index < entries.size(); ++index) {
      const auto& entry = entries[index];
      if (cancellation_requested(options.callbacks)) {
        return abort_with_error(target, cancelled_error());
      }
      if (entry.kind ==
          migrationcore::FileSystemRecreateEntryKind::directory) {
        auto create_guard = revalidate_mutation_boundary(
            plan,
            source,
            target,
            source_sequence,
            target_sequence,
            target_reidentification_count,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            true);
        if (!create_guard) {
          return abort_with_error(target, create_guard.error());
        }
        if (cancellation_requested(options.callbacks)) {
          return abort_with_error(target, cancelled_error());
        }
        auto created = target.create_directory_no_replace(
            create_guard.value(), entry);
        if (!created) {
          return abort_with_error(target, created.error());
        }
        const auto created_valid = validate_mutation_evidence(
            created.value(),
            create_guard.value(),
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            false);
        if (!created_valid) {
          return abort_with_error(target, created_valid.error());
        }
        ++directory_count;
        ++completed_units;
        if (safe_boundary_cancelled(
                options.callbacks,
                plan.core_plan().target_partition_number(),
                copied_file_bytes,
                completed_units)) {
          return abort_with_error(target, cancelled_error());
        }
        continue;
      }

      auto source_file = source.open_regular_file(index, entry);
      if (!source_file) {
        return abort_with_error(target, source_file.error());
      }
      if (source_file.value()->expected_size_bytes() != entry.size_bytes) {
        return abort_with_error(
            target,
            recreate_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_REVISION_MISMATCH,
                L"再作成source execution file size",
                L"再open source streamの固定sizeが計画entryと一致しません"));
      }
      auto create_guard = revalidate_mutation_boundary(
          plan,
          source,
          target,
          source_sequence,
          target_sequence,
          target_reidentification_count,
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive,
          true);
      if (!create_guard) {
        return abort_with_error(target, create_guard.error());
      }
      if (cancellation_requested(options.callbacks)) {
        return abort_with_error(target, cancelled_error());
      }
      auto created = target.create_file_no_replace(
          create_guard.value(), entry);
      if (!created) {
        return abort_with_error(target, created.error());
      }
      if (created.value().opened_handle_token == 0U) {
        return abort_with_error(
            target,
            recreate_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_HANDLE,
                L"再作成target no-replace file handle",
                L"no-replace作成後のretained file handle tokenが不正です"));
      }
      const auto file_created_valid = validate_mutation_evidence(
          created.value().mutation,
          create_guard.value(),
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive,
          false);
      if (!file_created_valid) {
        return abort_with_error(target, file_created_valid.error());
      }

      std::uint64_t file_offset{};
      for (;;) {
        auto bytes = source_file.value()->read_next(
            options.maximum_transfer_bytes);
        if (!bytes) {
          return abort_with_error(target, bytes.error());
        }
        if (bytes.value().empty()) {
          break;
        }
        if (bytes.value().size() > options.maximum_transfer_bytes ||
            file_offset > entry.size_bytes ||
            bytes.value().size() > entry.size_bytes - file_offset ||
            copied_file_bytes >
                (std::numeric_limits<std::uint64_t>::max)() -
                    bytes.value().size()) {
          return abort_with_error(
              target,
              recreate_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_ARITHMETIC_OVERFLOW,
                  L"再作成copy progress容量",
                  L"source chunk、file offset、または累積copy容量がtarget mutation前の固定範囲を超えました"));
        }
        if (cancellation_requested(options.callbacks)) {
          return abort_with_error(target, cancelled_error());
        }
        auto write_guard = revalidate_mutation_boundary(
            plan,
            source,
            target,
            source_sequence,
            target_sequence,
            target_reidentification_count,
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            true);
        if (!write_guard) {
          return abort_with_error(target, write_guard.error());
        }
        if (cancellation_requested(options.callbacks)) {
          return abort_with_error(target, cancelled_error());
        }
        auto written = target.write_file_chunk(
            write_guard.value(),
            created.value(),
            file_offset,
            bytes.value());
        if (!written) {
          return abort_with_error(target, written.error());
        }
        const auto write_valid = validate_mutation_evidence(
            written.value().mutation,
            write_guard.value(),
            FileSystemRecreateTargetIsolationState::
                construction_volume_online_exclusive,
            false);
        if (!write_valid) {
          return abort_with_error(target, write_valid.error());
        }
        if (written.value().bytes_written != bytes.value().size()) {
          return abort_with_error(
              target,
              recreate_error(
                  clonecore::ErrorCode::io_failed,
                  ERROR_WRITE_FAULT,
                  L"再作成target short write",
                  L"target stream writeが要求chunk全体を書き込みませんでした"));
        }
        file_offset += bytes.value().size();
        copied_file_bytes += bytes.value().size();
        report_progress(
            options,
            clonecore::DiskOperationStage::copying_data,
            plan.core_plan().target_partition_number(),
            plan.core_plan().capacity().total_content_bytes,
            copied_file_bytes,
            true);
      }
      const auto source_finished =
          source_file.value()->finish_and_verify();
      if (!source_finished) {
        return abort_with_error(target, source_finished.error());
      }
      if (file_offset != entry.size_bytes) {
        return abort_with_error(
            target,
            recreate_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_HANDLE_EOF,
                L"再作成source exact EOF copy",
                L"stable EOFまでのcopy容量がcanonical entry sizeと一致しません"));
      }
      auto finalize_guard = revalidate_mutation_boundary(
          plan,
          source,
          target,
          source_sequence,
          target_sequence,
          target_reidentification_count,
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive,
          true);
      if (!finalize_guard) {
        return abort_with_error(target, finalize_guard.error());
      }
      if (cancellation_requested(options.callbacks)) {
        return abort_with_error(target, cancelled_error());
      }
      auto finalized = target.finalize_file_metadata_flush_and_close(
          finalize_guard.value(), created.value(), entry);
      if (!finalized) {
        return abort_with_error(target, finalized.error());
      }
      const auto finalized_valid = validate_mutation_evidence(
          finalized.value(),
          finalize_guard.value(),
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive,
          true);
      if (!finalized_valid) {
        return abort_with_error(target, finalized_valid.error());
      }
      ++regular_file_count;
      ++completed_units;
      if (safe_boundary_cancelled(
              options.callbacks,
              plan.core_plan().target_partition_number(),
              copied_file_bytes,
              completed_units)) {
        return abort_with_error(target, cancelled_error());
      }
    }

    for (std::size_t reverse = entries.size(); reverse > 0U; --reverse) {
      const auto& entry = entries[reverse - 1U];
      if (entry.kind !=
          migrationcore::FileSystemRecreateEntryKind::directory) {
        continue;
      }
      if (cancellation_requested(options.callbacks)) {
        return abort_with_error(target, cancelled_error());
      }
      auto metadata_guard = revalidate_mutation_boundary(
          plan,
          source,
          target,
          source_sequence,
          target_sequence,
          target_reidentification_count,
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive,
          true);
      if (!metadata_guard) {
        return abort_with_error(target, metadata_guard.error());
      }
      if (cancellation_requested(options.callbacks)) {
        return abort_with_error(target, cancelled_error());
      }
      auto applied = target.apply_directory_metadata_and_flush(
          metadata_guard.value(), entry);
      if (!applied) {
        return abort_with_error(target, applied.error());
      }
      const auto applied_valid = validate_mutation_evidence(
          applied.value(),
          metadata_guard.value(),
          FileSystemRecreateTargetIsolationState::
              construction_volume_online_exclusive,
          true);
      if (!applied_valid) {
        return abort_with_error(target, applied_valid.error());
      }
    }

    auto flush_guard = revalidate_mutation_boundary(
        plan,
        source,
        target,
        source_sequence,
        target_sequence,
        target_reidentification_count,
        FileSystemRecreateTargetIsolationState::
            construction_volume_online_exclusive,
        true);
    if (!flush_guard) {
      return abort_with_error(target, flush_guard.error());
    }
    if (cancellation_requested(options.callbacks)) {
      return abort_with_error(target, cancelled_error());
    }
    auto flushed = target.flush_target_namespace(flush_guard.value());
    if (!flushed) {
      return abort_with_error(target, flushed.error());
    }
    const auto flushed_valid = validate_mutation_evidence(
        flushed.value(),
        flush_guard.value(),
        FileSystemRecreateTargetIsolationState::
            construction_volume_online_exclusive,
        true);
    if (!flushed_valid) {
      return abort_with_error(target, flushed_valid.error());
    }

    report_progress(
        options,
        clonecore::DiskOperationStage::verifying_final,
        plan.core_plan().target_partition_number(),
        plan.core_plan().capacity().total_content_bytes,
        copied_file_bytes,
        true);
    auto readback_boundary = revalidate_mutation_boundary(
        plan,
        source,
        target,
        source_sequence,
        target_sequence,
        target_reidentification_count,
        FileSystemRecreateTargetIsolationState::
            construction_volume_online_exclusive,
        true);
    if (!readback_boundary) {
      return abort_with_error(target, readback_boundary.error());
    }
    if (cancellation_requested(options.callbacks)) {
      return abort_with_error(target, cancelled_error());
    }
    auto readback = target.enumerate_complete_target_readback_read_only(
        plan.core_plan());
    if (!readback) {
      return abort_with_error(target, readback.error());
    }
    auto verification = migrationcore::verify_recreated_file_system_tree(
        plan.core_plan(), readback.value());
    if (!verification) {
      return abort_with_error(target, verification.error());
    }
    if (!verification.value().exact_tree_and_content_equivalence ||
        !verification.value().namespace_fully_enumerated ||
        !verification.value().every_regular_file_hashed_to_stable_eof ||
        verification.value().directory_count != directory_count ||
        verification.value().regular_file_count != regular_file_count ||
        verification.value().regular_file_bytes_read != copied_file_bytes) {
      return abort_with_error(
          target,
          recreate_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"再作成target full readback証跡",
              L"完全namespace、全file SHA-256、件数、または読戻し容量がexecution結果と一致しません"));
    }

    auto commit_guard = revalidate_mutation_boundary(
        plan,
        source,
        target,
        source_sequence,
        target_sequence,
        target_reidentification_count,
        FileSystemRecreateTargetIsolationState::
            construction_volume_online_exclusive,
        true);
    if (!commit_guard) {
      return abort_with_error(target, commit_guard.error());
    }
    if (cancellation_requested(options.callbacks)) {
      return abort_with_error(target, cancelled_error());
    }
    report_progress(
        options,
        clonecore::DiskOperationStage::committing_partition_table,
        plan.core_plan().target_partition_number(),
        plan.core_plan().capacity().total_content_bytes,
        copied_file_bytes,
        false);
    const auto committed = target.commit_completion_last(
        commit_guard.value(), verification.value());
    const auto& completion = committed.evidence;
    last_completion_evidence = completion;
    publication_may_have_started =
        committed.disposition !=
            FileSystemRecreateCommitDisposition::prepublication_failure ||
        completion.publication_attempted || completion.publication_latched;
    if (committed.disposition ==
        FileSystemRecreateCommitDisposition::prepublication_failure) {
      const auto error = recreate_error(
          committed.failure_code,
          committed.native_failure_code == ERROR_SUCCESS
              ? ERROR_WRITE_FAULT
              : committed.native_failure_code,
          L"再作成completion publication前失敗",
          L"completion publication開始前に失敗したためtargetをoffline/incompleteへ保持します");
      return publication_may_have_started
          ? partial_publication_outcome(completion, error)
          : abort_with_error(target, error);
    }
    if (committed.disposition == FileSystemRecreateCommitDisposition::
            partial_publication_use_prohibited) {
      return partial_publication_outcome(
          completion,
          recreate_error(
              clonecore::ErrorCode::verification_failed,
              committed.native_failure_code == ERROR_SUCCESS
                  ? ERROR_WRITE_FAULT
                  : committed.native_failure_code,
              L"再作成completion partial publication",
              completion.target_offline &&
                      completion.incomplete_use_prohibited
                  ? L"publication開始後に完了を証明できないためtargetをoffline/use-prohibitedのまま保持しました"
                  : L"publication開始後のoffline/use-prohibited証跡も完全ではないためtargetを使用できません"));
    }
    if (committed.disposition !=
            FileSystemRecreateCommitDisposition::completed ||
        !guard_equal(completion.accepted_guard, commit_guard.value()) ||
        completion.verified_manifest_sha256 !=
            verification.value().observed_manifest_sha256 ||
        completion.target_epoch_sha256 !=
            verification.value().target_epoch_sha256 ||
        !completion.guard_revalidated_inside_adapter ||
        !completion.exact_target_reidentified ||
        !completion.complete_readback_verified ||
        !completion.completion_committed_last ||
        !completion.target_offline || !completion.publication_attempted ||
        !completion.publication_latched ||
        !completion.publication_readback_verified ||
        completion.incomplete_use_prohibited) {
      return partial_publication_outcome(
          completion,
          recreate_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"再作成commit-last証跡",
              L"publication開始後のguard、完全読戻し、latch/readback、final commit順序、またはoffline証跡が不足しています。post-latch abortは実行しません"));
    }

    report_progress(
        options,
        clonecore::DiskOperationStage::completed,
        plan.core_plan().target_partition_number(),
        plan.core_plan().capacity().total_content_bytes,
        copied_file_bytes,
        false);
    auto report = FileSystemRecreateExecutionReport{
            .verification = verification.take_value(),
            .completion = completion,
            .directory_count = directory_count,
            .regular_file_count = regular_file_count,
            .copied_file_bytes = copied_file_bytes,
            .target_reidentification_count =
                target_reidentification_count,
            .every_mutation_guard_revalidated = true,
            .every_file_flushed = true,
            .full_namespace_read_back = true,
            .commit_was_last_mutation = true,
            .target_left_offline = true,
            .publication_latched = true,
            .cleanup_pending = completion.cleanup_pending,
            .incomplete_use_prohibited = false,
            .production_target_adapter_connected =
                kWindowsFileSystemRecreateProductionTargetAdapterConnected,
        };
    return clonecore::Result<FileSystemRecreateTransactionOutcome>::success(
        FileSystemRecreateTransactionOutcome{
            .disposition =
                FileSystemRecreateTransactionDisposition::completed,
            .completed_report = std::move(report),
            .completion = completion,
        });
  } catch (...) {
    const auto error = recreate_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"再作成target-only transaction例外",
        L"source/target adapterまたはcontrollerから例外が発生しました");
    if (!mutation_started) {
      return clonecore::Result<FileSystemRecreateTransactionOutcome>::failure(
          error);
    }
    return publication_may_have_started
        ? partial_publication_outcome(last_completion_evidence, error)
        : abort_with_error(target, error);
  }
}

}  // namespace ytec::migrationengine

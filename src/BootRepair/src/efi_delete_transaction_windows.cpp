#include "ytec/bootrepair/efi_delete_transaction_windows.h"

#include "ytec/bootrepair/efi_boot_ownership.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>
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

namespace ytec::bootrepair {
namespace {

constexpr std::size_t kDirectoryQueryBytes = 64U * 1024U;
constexpr std::size_t kHashBufferBytes = 64U * 1024U;
constexpr std::wstring_view kBcdRollbackSuffix =
    L".ytec-efi-delete-backup-v1";

constexpr ULONG kFileDirectoryFile = 0x00000001UL;
constexpr ULONG kFileNonDirectoryFile = 0x00000040UL;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020UL;
constexpr ULONG kFileOpenReparsePoint = 0x00200000UL;
constexpr ULONG kFileOpen = 0x00000001UL;
constexpr ULONG kFileCreate = 0x00000002UL;
constexpr NTSTATUS kStatusObjectNameCollision =
    static_cast<NTSTATUS>(0xC0000035UL);

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

clonecore::Error windows_delete_error(
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

bool same_text(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t left_character, const wchar_t right_character) {
            return std::towlower(left_character) ==
                std::towlower(right_character);
          });
}

bool same_bcd_store_object(
    const BcdStoreFileIdentity& left,
    const BcdStoreFileIdentity& right) noexcept {
  return left.volume_serial_number == right.volume_serial_number &&
      left.file_id == right.file_id && left.length == right.length;
}

std::wstring without_trailing_slash(std::wstring value) {
  while (!value.empty() &&
         (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return value;
}

std::wstring normalize_root(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  if (!value.empty() && value.back() != L'\\') {
    value.push_back(L'\\');
  }
  return value;
}

std::wstring guid_text(const GUID& guid) {
  std::array<wchar_t, 40> buffer{};
  const int length = StringFromGUID2(
      guid, buffer.data(), static_cast<int>(buffer.size()));
  return length > 1 ? std::wstring(buffer.data(), length - 1) : L"";
}

DWORD native_status_error(const NTSTATUS status) noexcept {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return ERROR_GEN_FAILURE;
  }
  const auto converter =
      reinterpret_cast<RtlNtStatusToDosErrorFunction>(
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
            L"EFI handle-relative API取得",
            GetLastError()));
  }
  const auto function = reinterpret_cast<NtCreateFileFunction>(
      GetProcAddress(ntdll, "NtCreateFile"));
  if (function == nullptr) {
    return clonecore::Result<NtCreateFileFunction>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::unsupported_platform,
            L"EFI handle-relative NtCreateFile取得",
            GetLastError()));
  }
  return clonecore::Result<NtCreateFileFunction>::success(function);
}

clonecore::Result<clonecore::UniqueHandle> open_relative_object(
    const HANDLE parent,
    const std::wstring_view name,
    const bool directory,
    const bool delete_access,
    const ULONG create_disposition = kFileOpen) {
  if (parent == nullptr || parent == INVALID_HANDLE_VALUE || name.empty() ||
      name.size() > kMaximumEfiDeleteNameCharacters ||
      name.size() >
          (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t)) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        windows_delete_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"EFI handle-relative open",
            L"親handleまたは相対名が安全な固定上限外です"));
  }
  auto native = nt_create_file_function();
  if (!native) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        native.error());
  }
  UNICODE_STRING relative{};
  relative.Buffer = const_cast<PWSTR>(name.data());
  relative.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
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
  ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
  access |= directory ? FILE_LIST_DIRECTORY : FILE_READ_DATA;
  if (delete_access) {
    access |= DELETE;
  }
  const ULONG options = kFileOpenReparsePoint |
      kFileSynchronousIoNonAlert |
      (directory ? kFileDirectoryFile : kFileNonDirectoryFile);
  const NTSTATUS status = native.value()(
      &opened,
      access,
      &attributes,
      &io,
      nullptr,
      directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
      FILE_SHARE_READ,
      create_disposition,
      options,
      nullptr,
      0U);
  if (status < 0) {
    const DWORD native_code = native_status_error(status);
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        windows_delete_error(
            native_code == ERROR_ALREADY_EXISTS ||
                    status == kStatusObjectNameCollision
                ? clonecore::ErrorCode::invalid_data
                : clonecore::ErrorCode::io_failed,
            native_code,
            create_disposition == kFileCreate
                ? L"EFI固定namespace作成"
                : L"EFI handle-relative open",
            create_disposition == kFileCreate
                ? L"同一ESPの固定namespaceをno-replaceで作成できませんでした"
                : L"親directory handleから対象を安全に開けませんでした"));
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      clonecore::UniqueHandle(opened));
}

struct DirectoryEntry final {
  std::wstring name;
  DWORD attributes{};
};

clonecore::Result<std::vector<DirectoryEntry>> enumerate_directory(
    const HANDLE directory) {
  std::vector<DirectoryEntry> entries;
  std::vector<std::byte> buffer(kDirectoryQueryBytes);
  bool restart = true;
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
      return clonecore::Result<std::vector<DirectoryEntry>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::enumeration_failed,
              L"EFI handle-bound directory列挙",
              native_code));
    }
    restart = false;
    std::size_t offset = 0U;
    for (;;) {
      if (offset > buffer.size() - sizeof(FILE_ID_BOTH_DIR_INFO)) {
        return clonecore::Result<std::vector<DirectoryEntry>>::failure(
            windows_delete_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"EFI directory列挙buffer",
                L"directory entryの境界が不正です"));
      }
      const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
          buffer.data() + offset);
      if ((info->FileNameLength % sizeof(wchar_t)) != 0U ||
          info->FileNameLength >
              kMaximumEfiDeleteNameCharacters * sizeof(wchar_t)) {
        return clonecore::Result<std::vector<DirectoryEntry>>::failure(
            windows_delete_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_NAME,
                L"EFI directory entry名",
                L"entry名が安全な固定上限外です"));
      }
      const std::wstring name(
          info->FileName,
          info->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        if (entries.size() >= kMaximumEfiDeleteTreeEntries) {
          return clonecore::Result<std::vector<DirectoryEntry>>::failure(
              windows_delete_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_BUFFER_OVERFLOW,
                  L"EFI directory entry件数",
                  L"directory列挙が固定上限を超えました"));
        }
        entries.push_back(DirectoryEntry{
            .name = name,
            .attributes = info->FileAttributes,
        });
      }
      if (info->NextEntryOffset == 0U) {
        break;
      }
      if (info->NextEntryOffset < sizeof(FILE_ID_BOTH_DIR_INFO) ||
          info->NextEntryOffset > buffer.size() - offset) {
        return clonecore::Result<std::vector<DirectoryEntry>>::failure(
            windows_delete_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"EFI directory entry連鎖",
                L"directory entry offsetが不正です"));
      }
      offset += info->NextEntryOffset;
    }
  }
  std::sort(
      entries.begin(),
      entries.end(),
      [](const auto& left, const auto& right) {
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
      });
  return clonecore::Result<std::vector<DirectoryEntry>>::success(
      std::move(entries));
}

bool is_directory_attributes(const DWORD attributes) noexcept {
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
}

bool is_reparse_attributes(const DWORD attributes) noexcept {
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

struct HandleMetadata final {
  EfiDeleteEntryKind kind{EfiDeleteEntryKind::unknown};
  std::uint64_t volume_serial{};
  EfiDeleteFileId file_id{};
  std::uint64_t size{};
  std::uint64_t creation_time{};
  std::uint64_t access_time{};
  std::uint64_t write_time{};
  std::uint64_t change_time{};
  std::uint32_t links{};
};

clonecore::Result<HandleMetadata> query_handle_metadata(
    const HANDLE handle,
    const bool expected_directory) {
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
    return clonecore::Result<HandleMetadata>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"EFI opened-handle metadata",
            GetLastError()));
  }
  const bool directory = standard.Directory != FALSE;
  if (standard.DeletePending != FALSE || directory != expected_directory ||
      is_reparse_attributes(attributes.FileAttributes) ||
      standard.NumberOfLinks != 1U || standard.EndOfFile.QuadPart < 0) {
    return clonecore::Result<HandleMetadata>::failure(
        windows_delete_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"EFI opened-handle object type",
            L"通常file/directory、単一link、非reparse、非delete-pendingを証明できません"));
  }
  HandleMetadata result{
      .kind = directory
          ? EfiDeleteEntryKind::directory
          : EfiDeleteEntryKind::regular_file,
      .volume_serial = identifier.VolumeSerialNumber,
      .size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .creation_time = static_cast<std::uint64_t>(
          basic.CreationTime.QuadPart),
      .access_time = static_cast<std::uint64_t>(
          basic.LastAccessTime.QuadPart),
      .write_time = static_cast<std::uint64_t>(
          basic.LastWriteTime.QuadPart),
      .change_time = static_cast<std::uint64_t>(
          basic.ChangeTime.QuadPart),
      .links = standard.NumberOfLinks,
  };
  static_assert(sizeof(result.file_id) == sizeof(identifier.FileId));
  std::memcpy(
      result.file_id.data(),
      identifier.FileId.Identifier,
      result.file_id.size());
  return clonecore::Result<HandleMetadata>::success(result);
}

bool same_metadata(
    const HandleMetadata& left,
    const HandleMetadata& right) noexcept {
  return left.kind == right.kind &&
      left.volume_serial == right.volume_serial &&
      left.file_id == right.file_id && left.size == right.size &&
      left.creation_time == right.creation_time &&
      left.access_time == right.access_time &&
      left.write_time == right.write_time &&
      left.change_time == right.change_time && left.links == right.links;
}

clonecore::Result<EfiDeleteSha256> hash_file_handle(const HANDLE file) {
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  ULONG object_size{};
  ULONG returned{};
  std::vector<UCHAR> object;
  auto digest = std::make_unique<EfiDeleteSha256>();
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
  if (status >= 0) {
    status = BCryptGetProperty(
        algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_size),
        sizeof(object_size),
        &returned,
        0U);
  }
  if (status >= 0 && object_size != 0U && algorithm != nullptr) {
    object.resize(object_size);
    status = BCryptCreateHash(
        algorithm,
        &hash,
        object.data(),
        object_size,
        nullptr,
        0U,
        0U);
  }
  if (status >= 0 && hash == nullptr) {
    status = static_cast<NTSTATUS>(0xC0000001UL);
  }
  LARGE_INTEGER zero{};
  if (status >= 0 && SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == FALSE) {
    status = static_cast<NTSTATUS>(0xC0000001UL);
  }
  auto buffer = std::make_unique<
      std::array<std::byte, kHashBufferBytes>>();
  while (status >= 0) {
    DWORD read{};
    if (ReadFile(
            file,
            buffer->data(),
            static_cast<DWORD>(buffer->size()),
            &read,
            nullptr) == FALSE) {
      status = static_cast<NTSTATUS>(0xC0000001UL);
      break;
    }
    if (read == 0U) {
      break;
    }
    if (hash == nullptr) {
      status = static_cast<NTSTATUS>(0xC0000001UL);
    } else {
      status = BCryptHashData(
          hash,
          reinterpret_cast<PUCHAR>(buffer->data()),
          read,
          0U);
    }
  }
  if (status >= 0 && hash != nullptr) {
    status = BCryptFinishHash(
        hash,
        reinterpret_cast<PUCHAR>(digest->data()),
        static_cast<ULONG>(digest->size()),
        0U);
  }
  if (hash != nullptr) {
    BCryptDestroyHash(hash);
  }
  if (algorithm != nullptr) {
    BCryptCloseAlgorithmProvider(algorithm, 0U);
  }
  if (status < 0) {
    return clonecore::Result<EfiDeleteSha256>::failure(
        windows_delete_error(
            clonecore::ErrorCode::verification_failed,
            native_status_error(status),
            L"EFI file handle SHA-256",
            L"同一handleからfile SHA-256を計算できませんでした"));
  }
  return clonecore::Result<EfiDeleteSha256>::success(*digest);
}

bool safe_relative_name(const std::wstring_view name) noexcept {
  if (name.empty() || name == L"." || name == L".." ||
      name.size() > kMaximumEfiDeleteNameCharacters) {
    return false;
  }
  return std::all_of(
      name.begin(), name.end(), [](const wchar_t character) {
        return character >= 0x21 && character <= 0x7e &&
            character != L'/' && character != L'\\' &&
            character != L':' && character != L'*' &&
            character != L'?' && character != L'"' &&
            character != L'<' && character != L'>' &&
            character != L'|';
      });
}

bool same_directory_census(
    const std::vector<DirectoryEntry>& left,
    const std::vector<DirectoryEntry>& right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(), left.end(), right.begin(),
          [](const DirectoryEntry& left_entry,
             const DirectoryEntry& right_entry) {
            return same_text(left_entry.name, right_entry.name) &&
                left_entry.attributes == right_entry.attributes;
          });
}

bool is_canonical_volume_guid_root(const std::wstring_view root) noexcept {
  if (root.size() < 12U || root.back() != L'\\' ||
      root.substr(0U, 10U) != L"\\\\?\\Volume{" ||
      root.find_first_of(L"/", 0U) != std::wstring_view::npos) {
    return false;
  }
  const std::size_t close = root.find(L'}');
  return close == root.size() - 2U &&
      root.find(L'\\', 4U) == root.size() - 1U;
}

clonecore::Result<clonecore::UniqueHandle> open_volume_root_directory(
    const std::wstring& volume_guid_root) {
  if (!is_canonical_volume_guid_root(volume_guid_root)) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        windows_delete_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"EFI Volume GUID root",
            L"ESPはcanonical Volume GUID rootで指定する必要があります"));
  }
  const std::wstring path = without_trailing_slash(volume_guid_root);
  clonecore::UniqueHandle root(CreateFileW(
      path.c_str(),
      FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!root) {
    return clonecore::Result<clonecore::UniqueHandle>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"EFI Volume GUID handle open",
            GetLastError()));
  }
  return clonecore::Result<clonecore::UniqueHandle>::success(
      std::move(root));
}

struct ObservedWindowsEsp final {
  clonecore::StableDiskIdentity disk;
  EfiDeleteEspIdentity esp;
};

clonecore::Result<ObservedWindowsEsp> observe_windows_esp_read_only(
    const WindowsEfiDeleteEspRequest& request) {
  if (!is_canonical_volume_guid_root(request.expected_volume_guid_root) ||
      request.expected_partition_number == 0U ||
      request.expected_length_bytes == 0U ||
      request.expected_partition_identifier.empty() ||
      request.expected_partition_type_identifier.empty()) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        windows_delete_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"EFI ESP再識別request",
            L"partition geometry、GPT identifiers、Volume GUIDが完全ではありません"));
  }

  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (!inventory) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        windows_delete_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"EFI disk inventory",
            L"読取り専用disk inventoryを作成できませんでした"));
  }
  auto disk = diskmodel::reidentify_read_only_physical_disk(
      request.expected_disk, *inventory);
  if (!disk) {
    return clonecore::Result<ObservedWindowsEsp>::failure(disk.error());
  }
  const auto partition = std::find_if(
      disk.value().observed.partitions.begin(),
      disk.value().observed.partitions.end(),
      [&](const diskmodel::PartitionInfo& value) {
        return value.number == request.expected_partition_number;
      });
  if (partition == disk.value().observed.partitions.end() ||
      partition->style != diskmodel::PartitionStyle::gpt ||
      partition->offset_bytes != request.expected_offset_bytes ||
      partition->size_bytes != request.expected_length_bytes ||
      !same_text(partition->identifier,
                 request.expected_partition_identifier) ||
      !same_text(partition->type,
                 request.expected_partition_type_identifier)) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"EFI ESP inventory再識別",
            L"disk inventoryのGPT partition identityがレビュー値と一致しません"));
  }

  const std::wstring volume_path =
      without_trailing_slash(request.expected_volume_guid_root);
  clonecore::UniqueHandle volume(CreateFileW(
      volume_path.c_str(),
      FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!volume) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"EFI ESP volume metadata handle",
            GetLastError()));
  }

  VOLUME_DISK_EXTENTS extents{};
  DWORD returned{};
  if (DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0U,
          &extents,
          sizeof(extents),
          &returned,
          nullptr) == FALSE ||
      extents.NumberOfDiskExtents != 1U ||
      extents.Extents[0].DiskNumber != disk.value().observed.disk_number ||
      extents.Extents[0].StartingOffset.QuadPart < 0 ||
      extents.Extents[0].ExtentLength.QuadPart < 0 ||
      static_cast<std::uint64_t>(
          extents.Extents[0].StartingOffset.QuadPart) !=
          request.expected_offset_bytes ||
      static_cast<std::uint64_t>(
          extents.Extents[0].ExtentLength.QuadPart) !=
          request.expected_length_bytes) {
    const DWORD native_code = GetLastError();
    return clonecore::Result<ObservedWindowsEsp>::failure(
        windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            native_code == ERROR_SUCCESS
                ? ERROR_DEVICE_NOT_CONNECTED
                : native_code,
            L"EFI ESP single extent再識別",
            L"Volume GUIDがレビュー済みdisk/partitionの単一extentへ一致しません"));
  }

  PARTITION_INFORMATION_EX native_partition{};
  if (DeviceIoControl(
          volume.get(),
          IOCTL_DISK_GET_PARTITION_INFO_EX,
          nullptr,
          0U,
          &native_partition,
          sizeof(native_partition),
          &returned,
          nullptr) == FALSE) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"EFI ESP GPT attributes query",
            GetLastError()));
  }
  const std::wstring partition_identifier =
      guid_text(native_partition.Gpt.PartitionId);
  const std::wstring partition_type =
      guid_text(native_partition.Gpt.PartitionType);
  if (native_partition.PartitionStyle != PARTITION_STYLE_GPT ||
      native_partition.PartitionNumber != request.expected_partition_number ||
      native_partition.StartingOffset.QuadPart < 0 ||
      native_partition.PartitionLength.QuadPart < 0 ||
      static_cast<std::uint64_t>(
          native_partition.StartingOffset.QuadPart) !=
          request.expected_offset_bytes ||
      static_cast<std::uint64_t>(
          native_partition.PartitionLength.QuadPart) !=
          request.expected_length_bytes ||
      !same_text(partition_identifier,
                 request.expected_partition_identifier) ||
      !same_text(partition_type,
                 request.expected_partition_type_identifier)) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"EFI ESP GPT identity再照合",
            L"Volume handleのGPT type/id/geometryがレビュー値と一致しません"));
  }

  DWORD serial{};
  std::array<wchar_t, 32> file_system{};
  if (GetVolumeInformationW(
          request.expected_volume_guid_root.c_str(),
          nullptr,
          0U,
          &serial,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size())) == FALSE) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"EFI ESP filesystem query",
            GetLastError()));
  }
  if (!same_text(file_system.data(), L"FAT32")) {
    return clonecore::Result<ObservedWindowsEsp>::failure(
        windows_delete_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"EFI ESP filesystem",
            L"FAT32として証明できないpartitionはEFI削除対象にしません"));
  }

  return clonecore::Result<ObservedWindowsEsp>::success(
      ObservedWindowsEsp{
          .disk = disk.value().identity,
          .esp = EfiDeleteEspIdentity{
              .partition_number = native_partition.PartitionNumber,
              .offset_bytes = static_cast<std::uint64_t>(
                  native_partition.StartingOffset.QuadPart),
              .length_bytes = static_cast<std::uint64_t>(
                  native_partition.PartitionLength.QuadPart),
              .partition_identifier = partition_identifier,
              .partition_type_identifier = partition_type,
              .partition_attributes = native_partition.Gpt.Attributes,
              .volume_guid_root = normalize_root(
                  request.expected_volume_guid_root),
              .filesystem_name = file_system.data(),
              .volume_serial_number = serial,
          },
      });
}

EfiDeleteTreeEntryObservation make_entry_observation(
    const HandleMetadata& metadata,
    std::wstring relative_path,
    const std::optional<EfiDeleteSha256>& digest) {
  return EfiDeleteTreeEntryObservation{
      .kind = metadata.kind,
      .relative_path = std::move(relative_path),
      .volume_serial_number = metadata.volume_serial,
      .file_id = metadata.file_id,
      .file_id_valid = true,
      .size_bytes = metadata.size,
      .creation_time = metadata.creation_time,
      .last_access_time = metadata.access_time,
      .last_write_time = metadata.write_time,
      .change_time = metadata.change_time,
      .hard_link_count = metadata.links,
      .file_sha256 = digest.value_or(EfiDeleteSha256{}),
      .file_sha256_valid = digest.has_value(),
  };
}

clonecore::Status scan_tree_handle(
    const HANDLE object,
    const bool directory,
    const bool request_delete_access,
    const std::wstring& relative_path,
    const std::size_t depth,
    std::vector<EfiDeleteTreeEntryObservation>& entries) {
  if (depth > kMaximumEfiDeleteTreeDepth ||
      relative_path.size() > kMaximumEfiDeleteRelativePathCharacters ||
      entries.size() >= kMaximumEfiDeleteTreeEntries) {
    return clonecore::Status::failure(windows_delete_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"EFI candidate tree bounds",
        L"candidate treeが固定深さ、件数、またはpath長上限を超えました"));
  }
  auto before = query_handle_metadata(object, directory);
  if (!before) {
    return clonecore::Status::failure(before.error());
  }

  std::optional<EfiDeleteSha256> digest;
  if (!directory) {
    auto hashed = hash_file_handle(object);
    if (!hashed) {
      return clonecore::Status::failure(hashed.error());
    }
    digest = hashed.take_value();
  }
  entries.push_back(make_entry_observation(
      before.value(), relative_path, digest));

  if (directory) {
    auto children = enumerate_directory(object);
    if (!children) {
      return clonecore::Status::failure(children.error());
    }
    for (const auto& child : children.value()) {
      if (!safe_relative_name(child.name) ||
          is_reparse_attributes(child.attributes)) {
        return clonecore::Status::failure(windows_delete_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"EFI candidate child",
            L"禁止名またはreparse objectをcandidate treeで検出しました"));
      }
      const bool child_directory = is_directory_attributes(child.attributes);
      auto opened = open_relative_object(
          object, child.name, child_directory, request_delete_access);
      if (!opened) {
        return clonecore::Status::failure(opened.error());
      }
      const std::wstring child_path = relative_path + L"\\" + child.name;
      const auto child_status = scan_tree_handle(
          opened.value().get(),
          child_directory,
          request_delete_access,
          child_path,
          depth + 1U,
          entries);
      if (!child_status) {
        return child_status;
      }
    }
    auto after_children = enumerate_directory(object);
    if (!after_children ||
        !same_directory_census(children.value(), after_children.value())) {
      return clonecore::Status::failure(
          after_children
              ? windows_delete_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_REVISION_MISMATCH,
                    L"EFI candidate directory再列挙",
                    L"handle-bound列挙中にdirectory内容が変化しました")
              : after_children.error());
    }
  }

  auto after = query_handle_metadata(object, directory);
  if (!after || !same_metadata(before.value(), after.value())) {
    return clonecore::Status::failure(
        after
            ? windows_delete_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_REVISION_MISMATCH,
                  L"EFI candidate metadata再照合",
                  L"handle-bound hash/列挙中にobject metadataが変化しました")
            : after.error());
  }
  return clonecore::success_status();
}

struct ScannedWindowsEfiTarget final {
  EfiDeleteReviewObservation observation;
  clonecore::UniqueHandle volume_root;
  clonecore::UniqueHandle efi_root;
  std::vector<clonecore::UniqueHandle> candidate_roots;
};

clonecore::Result<ScannedWindowsEfiTarget> scan_windows_efi_target(
    const clonecore::StableDiskIdentity& expected_disk,
    const EfiDeleteEspIdentity& expected_esp,
    const bool retain_delete_handles) {
  WindowsEfiDeleteEspRequest request{
      .expected_disk = expected_disk,
      .expected_partition_number = expected_esp.partition_number,
      .expected_offset_bytes = expected_esp.offset_bytes,
      .expected_length_bytes = expected_esp.length_bytes,
      .expected_partition_identifier = expected_esp.partition_identifier,
      .expected_partition_type_identifier =
          expected_esp.partition_type_identifier,
      .expected_volume_guid_root = expected_esp.volume_guid_root,
  };
  auto target = observe_windows_esp_read_only(request);
  if (!target) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        target.error());
  }
  if (!equivalent_efi_delete_esp_identity(
          expected_esp, target.value().esp)) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"EFI ESP exact再識別",
            L"GPT attributes、FAT32、volume serialを含むESP identityが一致しません"));
  }

  auto ownership_inspector = make_windows_efi_boot_ownership_inspector();
  if (!ownership_inspector) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        windows_delete_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"EFI ownership inspector",
            L"読取り専用EFI ownership inspectorを作成できませんでした"));
  }
  auto ownership_before =
      ownership_inspector->inspect_existing_esp_read_only(
          target.value().esp.volume_guid_root);
  if (!ownership_before ||
      !efi_boot_ownership_allows_third_party_preserve(
          ownership_before.value())) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        ownership_before
            ? windows_delete_error(
                  clonecore::ErrorCode::unsupported_layout,
                  ERROR_NOT_SUPPORTED,
                  L"EFI ownership delete boundary",
                  L"独立top-level通常directoryだけの第三者EFI状態ではありません")
            : ownership_before.error());
  }

  auto volume_root = open_volume_root_directory(
      target.value().esp.volume_guid_root);
  if (!volume_root) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        volume_root.error());
  }
  auto efi_root = open_relative_object(
      volume_root.value().get(), L"EFI", true, false);
  if (!efi_root) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        efi_root.error());
  }
  auto census_before = enumerate_directory(efi_root.value().get());
  if (!census_before) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        census_before.error());
  }

  std::vector<EfiDeleteCandidateObservation> candidates;
  std::vector<clonecore::UniqueHandle> candidate_roots;
  for (const auto& child : census_before.value()) {
    if (same_text(child.name, L"Microsoft") ||
        same_text(child.name, L"Boot")) {
      continue;
    }
    if (!safe_relative_name(child.name) ||
        !is_directory_attributes(child.attributes) ||
        is_reparse_attributes(child.attributes) ||
        candidates.size() >= kMaximumEfiDeleteCandidates) {
      return clonecore::Result<ScannedWindowsEfiTarget>::failure(
          windows_delete_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"EFI top-level delete candidate",
              L"Microsoft/Boot以外に独立した通常directoryではないobjectを検出しました"));
    }
    auto candidate = open_relative_object(
        efi_root.value().get(),
        child.name,
        true,
        retain_delete_handles);
    if (!candidate) {
      return clonecore::Result<ScannedWindowsEfiTarget>::failure(
          candidate.error());
    }
    EfiDeleteCandidateObservation observation{
        .relative_name = child.name,
    };
    auto status = scan_tree_handle(
        candidate.value().get(),
        true,
        retain_delete_handles,
        child.name,
        0U,
        observation.entries);
    if (!status) {
      return clonecore::Result<ScannedWindowsEfiTarget>::failure(
          status.error());
    }
    candidates.push_back(std::move(observation));
    candidate_roots.push_back(candidate.take_value());
  }
  if (candidates.empty() ||
      candidates.size() !=
          ownership_before.value().top_level_non_microsoft_namespace_count) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_REVISION_MISMATCH,
            L"EFI candidate ownership census",
            L"ownership分類とhandle-bound candidate件数が一致しません"));
  }

  auto census_after = enumerate_directory(efi_root.value().get());
  auto ownership_after =
      ownership_inspector->inspect_existing_esp_read_only(
          target.value().esp.volume_guid_root);
  if (!census_after ||
      !same_directory_census(census_before.value(), census_after.value()) ||
      !ownership_after ||
      !equivalent_efi_boot_ownership(
          ownership_before.value(), ownership_after.value())) {
    return clonecore::Result<ScannedWindowsEfiTarget>::failure(
        !census_after
            ? census_after.error()
            : !ownership_after
                ? ownership_after.error()
                : windows_delete_error(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_REVISION_MISMATCH,
                      L"EFI stable top-level review",
                      L"tree review中にEFI top-levelまたはownershipが変化しました"));
  }

  return clonecore::Result<ScannedWindowsEfiTarget>::success(
      ScannedWindowsEfiTarget{
          .observation = EfiDeleteReviewObservation{
              .disk = target.value().disk,
              .esp = target.value().esp,
              .ownership = ownership_after.value(),
              .bounded_top_level_enumeration_complete = true,
              .candidates = std::move(candidates),
          },
          .volume_root = volume_root.take_value(),
          .efi_root = efi_root.take_value(),
          .candidate_roots = std::move(candidate_roots),
      });
}

bool same_manifest_entry(
    const EfiDeleteTreeEntryObservation& observed,
    const EfiDeleteTreeEntryManifest& expected) noexcept {
  return observed.kind == expected.kind &&
      same_text(observed.relative_path, expected.relative_path) &&
      observed.volume_serial_number == expected.volume_serial_number &&
      observed.file_id == expected.file_id &&
      observed.size_bytes == expected.size_bytes &&
      observed.creation_time == expected.creation_time &&
      observed.last_access_time == expected.last_access_time &&
      observed.last_write_time == expected.last_write_time &&
      observed.change_time == expected.change_time &&
      observed.hard_link_count == expected.hard_link_count &&
      observed.file_sha256_valid == expected.has_file_sha256 &&
      observed.file_sha256 == expected.file_sha256;
}

clonecore::Status verify_candidate_tree_handle(
    const HANDLE candidate_root,
    const EfiDeleteCandidateManifest& candidate) {
  std::vector<EfiDeleteTreeEntryObservation> observed;
  const auto scanned = scan_tree_handle(
      candidate_root,
      true,
      true,
      candidate.relative_name,
      0U,
      observed);
  if (!scanned) {
    return scanned;
  }
  std::sort(
      observed.begin(), observed.end(),
      [](const auto& left, const auto& right) {
        return _wcsicmp(
                   left.relative_path.c_str(),
                   right.relative_path.c_str()) < 0;
      });
  if (observed.size() != candidate.entries.size() ||
      !std::equal(
          observed.begin(), observed.end(), candidate.entries.begin(),
          same_manifest_entry)) {
    return clonecore::Status::failure(windows_delete_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_REVISION_MISMATCH,
        L"EFI quarantined tree exact readback",
        L"FileId、type、size、times、SHA-256を含むtree manifestが一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status rename_handle_no_replace(
    const HANDLE object,
    const HANDLE destination_parent,
    const std::wstring_view destination_name) {
  if (!safe_relative_name(destination_name)) {
    return clonecore::Status::failure(windows_delete_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"EFI handle-bound rename destination",
        L"rename先の単一相対名が安全上限外です"));
  }
  const DWORD name_bytes = static_cast<DWORD>(
      destination_name.size() * sizeof(wchar_t));
  std::vector<std::byte> buffer(
      offsetof(FILE_RENAME_INFO, FileName) + name_bytes);
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = destination_parent;
  rename->FileNameLength = name_bytes;
  std::memcpy(rename->FileName, destination_name.data(), name_bytes);
  if (SetFileInformationByHandle(
          object,
          FileRenameInfo,
          rename,
          static_cast<DWORD>(buffer.size())) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"EFI handle-bound no-replace rename",
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<std::wstring> query_handle_name(const HANDLE object) {
  std::vector<std::byte> buffer(
      sizeof(FILE_NAME_INFO) +
      (kMaximumEfiDeleteRelativePathCharacters + 64U) * sizeof(wchar_t));
  if (GetFileInformationByHandleEx(
          object,
          FileNameInfo,
          buffer.data(),
          static_cast<DWORD>(buffer.size())) == FALSE) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"EFI renamed handle name readback",
            GetLastError()));
  }
  const auto* info = reinterpret_cast<const FILE_NAME_INFO*>(buffer.data());
  if ((info->FileNameLength % sizeof(wchar_t)) != 0U ||
      info->FileNameLength > buffer.size() - offsetof(FILE_NAME_INFO, FileName)) {
    return clonecore::Result<std::wstring>::failure(windows_delete_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"EFI renamed handle name buffer",
        L"handle name readbackの境界が不正です"));
  }
  return clonecore::Result<std::wstring>::success(std::wstring(
      info->FileName,
      info->FileNameLength / sizeof(wchar_t)));
}

bool name_has_suffix(
    const std::wstring_view full_name,
    const std::wstring_view expected_suffix) noexcept {
  return full_name.size() >= expected_suffix.size() &&
      same_text(
          full_name.substr(full_name.size() - expected_suffix.size()),
          expected_suffix);
}

clonecore::Status mark_handle_for_delete(const HANDLE object) {
  FILE_DISPOSITION_INFO disposition{
      .DeleteFile = TRUE,
  };
  if (SetFileInformationByHandle(
          object,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition)) == FALSE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"EFI handle-bound disposition",
        GetLastError()));
  }
  return clonecore::success_status();
}

const EfiDeleteTreeEntryManifest* find_manifest_entry(
    const EfiDeleteCandidateManifest& candidate,
    const std::wstring_view relative_path) noexcept {
  const auto found = std::find_if(
      candidate.entries.begin(), candidate.entries.end(),
      [&](const EfiDeleteTreeEntryManifest& entry) {
        return same_text(entry.relative_path, relative_path);
      });
  return found == candidate.entries.end() ? nullptr : &*found;
}

clonecore::Status verify_opened_entry(
    const HANDLE object,
    const bool directory,
    const EfiDeleteTreeEntryManifest& expected) {
  auto metadata = query_handle_metadata(object, directory);
  if (!metadata) {
    return clonecore::Status::failure(metadata.error());
  }
  std::optional<EfiDeleteSha256> digest;
  if (!directory) {
    auto hashed = hash_file_handle(object);
    if (!hashed) {
      return clonecore::Status::failure(hashed.error());
    }
    digest = hashed.take_value();
    auto after = query_handle_metadata(object, false);
    if (!after || !same_metadata(metadata.value(), after.value())) {
      return clonecore::Status::failure(
          after
              ? windows_delete_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_REVISION_MISMATCH,
                    L"EFI delete-entry final hash",
                    L"最終hash中にfile metadataが変化しました")
              : after.error());
    }
  }
  return same_manifest_entry(
      make_entry_observation(
          metadata.value(), expected.relative_path, digest),
      expected)
      ? clonecore::success_status()
      : clonecore::Status::failure(windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_REVISION_MISMATCH,
            L"EFI delete-entry final identity",
            L"削除直前のhandle identity/hashがmanifestと一致しません"));
}

clonecore::Status recursively_delete_manifest_tree(
    const HANDLE directory,
    const EfiDeleteCandidateManifest& candidate,
    const std::wstring& directory_path,
    const bool delete_root) {
  const auto* expected_directory =
      find_manifest_entry(candidate, directory_path);
  if (expected_directory == nullptr ||
      expected_directory->kind != EfiDeleteEntryKind::directory) {
    return clonecore::Status::failure(windows_delete_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_PATH_NOT_FOUND,
        L"EFI recursive delete manifest directory",
        L"opened directoryに対応するmanifest entryがありません"));
  }
  const auto verified =
      verify_opened_entry(directory, true, *expected_directory);
  if (!verified) {
    return verified;
  }

  auto children = enumerate_directory(directory);
  if (!children) {
    return clonecore::Status::failure(children.error());
  }
  std::size_t expected_child_count{};
  const std::wstring prefix = directory_path + L"\\";
  for (const auto& entry : candidate.entries) {
    if (!entry.relative_path.starts_with(prefix)) {
      continue;
    }
    const std::wstring_view remainder(
        entry.relative_path.data() + prefix.size(),
        entry.relative_path.size() - prefix.size());
    if (remainder.find(L'\\') == std::wstring_view::npos) {
      ++expected_child_count;
    }
  }
  if (children.value().size() != expected_child_count) {
    return clonecore::Status::failure(windows_delete_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_REVISION_MISMATCH,
        L"EFI recursive delete child census",
        L"削除直前のdirect child件数がmanifestと一致しません"));
  }
  for (const auto& child : children.value()) {
    const std::wstring child_path = prefix + child.name;
    const auto* expected = find_manifest_entry(candidate, child_path);
    const bool child_directory = is_directory_attributes(child.attributes);
    if (expected == nullptr ||
        child_directory !=
            (expected->kind == EfiDeleteEntryKind::directory) ||
        is_reparse_attributes(child.attributes)) {
      return clonecore::Status::failure(windows_delete_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"EFI recursive delete child mapping",
          L"削除直前のdirect child名/typeがmanifestと一致しません"));
    }
    auto opened = open_relative_object(
        directory, child.name, child_directory, true);
    if (!opened) {
      return clonecore::Status::failure(opened.error());
    }
    clonecore::Status status = child_directory
        ? recursively_delete_manifest_tree(
              opened.value().get(), candidate, child_path, true)
        : verify_opened_entry(
              opened.value().get(), false, *expected);
    if (!status) {
      return status;
    }
    if (!child_directory) {
      status = mark_handle_for_delete(opened.value().get());
      if (!status) {
        return status;
      }
    }
    opened.value().reset();
  }
  if (delete_root) {
    auto empty = enumerate_directory(directory);
    if (!empty || !empty.value().empty()) {
      return clonecore::Status::failure(
          empty
              ? windows_delete_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_DIR_NOT_EMPTY,
                    L"EFI recursive delete empty readback",
                    L"全childのhandle deletion後もdirectoryが空ではありません")
              : empty.error());
    }
    return mark_handle_for_delete(directory);
  }
  return clonecore::success_status();
}

EfiDeletePlatformFailureKind platform_failure_from_error(
    const clonecore::Error& error) noexcept {
  switch (error.code) {
    case clonecore::ErrorCode::identity_mismatch:
      return EfiDeletePlatformFailureKind::tamper_detected;
    case clonecore::ErrorCode::verification_failed:
      return EfiDeletePlatformFailureKind::verification_failure;
    case clonecore::ErrorCode::invalid_data:
    case clonecore::ErrorCode::unsupported_layout:
      return error.native_code == ERROR_ALREADY_EXISTS
          ? EfiDeletePlatformFailureKind::foreign_object
          : EfiDeletePlatformFailureKind::tamper_detected;
    case clonecore::ErrorCode::enumeration_failed:
      return EfiDeletePlatformFailureKind::race_detected;
    default:
      return EfiDeletePlatformFailureKind::io_failure;
  }
}

EfiDeletePlatformStepResult failed_step(
    const clonecore::Error& error,
    const EfiDeleteMutationExtent extent) {
  return EfiDeletePlatformStepResult::failed(
      platform_failure_from_error(error), extent, error);
}

clonecore::Error injected_error(const WindowsEfiDeleteFailurePoint point) {
  return windows_delete_error(
      clonecore::ErrorCode::io_failed,
      ERROR_GEN_FAILURE,
      L"EFI delete failure injection",
      L"focused test用の明示的failure pointです: " +
          std::to_wstring(static_cast<unsigned int>(point)));
}

bool injection_matches(
    const WindowsEfiDeleteFailureInjection& injection,
    const WindowsEfiDeleteFailurePoint point,
    const std::optional<std::size_t> candidate_index = std::nullopt) noexcept {
  return injection.point == point &&
      (!injection.candidate_index.has_value() ||
       injection.candidate_index == candidate_index);
}

clonecore::Result<std::wstring> trusted_system_directory() {
  std::vector<wchar_t> buffer(512U);
  for (;;) {
    const UINT length = GetSystemDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0U) {
      return clonecore::Result<std::wstring>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"EFI delete trusted System32",
              GetLastError()));
    }
    if (length < buffer.size()) {
      return clonecore::Result<std::wstring>::success(
          std::wstring(buffer.data(), length));
    }
    if (length > 32'767U) {
      return clonecore::Result<std::wstring>::failure(
          windows_delete_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_BUFFER_OVERFLOW,
              L"EFI delete trusted System32",
              L"System32 pathが固定上限を超えています"));
    }
    buffer.resize(static_cast<std::size_t>(length) + 1U);
  }
}

clonecore::Status verify_system_root_maps_to_esp(
    const std::wstring& system_root,
    const EfiDeleteEspIdentity& expected_esp) {
  const std::wstring normalized = normalize_root(system_root);
  std::array<wchar_t, 64> volume_name{};
  if (GetVolumeNameForVolumeMountPointW(
          normalized.c_str(),
          volume_name.data(),
          static_cast<DWORD>(volume_name.size())) == FALSE) {
    return clonecore::Status::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"EFI delete BCD system root mapping",
            GetLastError()));
  }
  return same_text(normalize_root(volume_name.data()),
                   expected_esp.volume_guid_root)
      ? clonecore::success_status()
      : clonecore::Status::failure(windows_delete_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"EFI delete BCD system root mapping",
            L"BCDBoot /s rootがレビュー済みESP Volume GUIDに一致しません"));
}

class WindowsEfiDeleteReadOnlyInspector final
    : public IEfiDeleteReadOnlyInspector {
 public:
  clonecore::Result<EfiDeleteReviewObservation>
  inspect_candidates_read_only(
      const clonecore::StableDiskIdentity& expected_disk,
      const EfiDeleteEspIdentity& expected_esp) override {
    auto scanned = scan_windows_efi_target(
        expected_disk, expected_esp, false);
    if (!scanned) {
      return clonecore::Result<EfiDeleteReviewObservation>::failure(
          scanned.error());
    }
    return clonecore::Result<EfiDeleteReviewObservation>::success(
        std::move(scanned.value().observation));
  }
};

enum class CandidateLocation : std::uint8_t {
  original,
  quarantined,
  deleted,
};

class WindowsEfiDeleteTransactionPlatform final
    : public IWindowsEfiDeleteTransactionPlatform {
 public:
  WindowsEfiDeleteTransactionPlatform(
      std::vector<BcdBootRequest> requests,
      WindowsEfiDeleteFailureInjection injection)
      : requests_(std::move(requests)), injection_(injection) {}

  clonecore::Result<EfiDeleteReviewObservation>
  inspect_candidates_read_only(
      const clonecore::StableDiskIdentity& expected_disk,
      const EfiDeleteEspIdentity& expected_esp) override {
    if (quarantine_ || bcd_active_) {
      return clonecore::Result<EfiDeleteReviewObservation>::failure(
          windows_delete_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"EFI delete fresh inspection state",
              L"mutation開始後にfresh inspectionを再利用できません"));
    }
    auto scanned = scan_windows_efi_target(
        expected_disk, expected_esp, true);
    if (!scanned) {
      return clonecore::Result<EfiDeleteReviewObservation>::failure(
          scanned.error());
    }
    auto retained_review =
        review_efi_delete_candidates(scanned.value().observation);
    if (!retained_review) {
      return clonecore::Result<EfiDeleteReviewObservation>::failure(
          retained_review.error());
    }
    retained_review_ = retained_review.take_value();
    volume_root_ = std::move(scanned.value().volume_root);
    efi_root_ = std::move(scanned.value().efi_root);
    candidate_roots_ = std::move(scanned.value().candidate_roots);
    locations_.assign(
        candidate_roots_.size(), CandidateLocation::original);
    return clonecore::Result<EfiDeleteReviewObservation>::success(
        std::move(scanned.value().observation));
  }

  EfiDeleteQuarantineCreateResult create_owned_quarantine_no_replace(
      const ReviewedEfiDeletePlan& reviewed) override {
    const auto valid = validate_review(reviewed);
    if (!valid) {
      return failed_quarantine(valid.error(), EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_quarantine_create)) {
      return failed_quarantine(
          injected_error(injection_.point), EfiDeleteMutationExtent::none);
    }
    auto created = open_relative_object(
        volume_root_.get(),
        efi_delete_quarantine_namespace(),
        true,
        true,
        kFileCreate);
    if (!created) {
      return failed_quarantine(
          created.error(), EfiDeleteMutationExtent::none);
    }
    auto metadata = query_handle_metadata(created.value().get(), true);
    if (!metadata ||
        metadata.value().volume_serial !=
            reviewed.expected_esp().volume_serial_number) {
      return failed_quarantine(
          metadata
              ? windows_delete_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_NOT_CONNECTED,
                    L"EFI quarantine identity",
                    L"作成したquarantineがレビュー済みESP上ではありません")
              : metadata.error(),
          EfiDeleteMutationExtent::partial_or_unknown);
    }
    quarantine_identity_ = EfiDeleteObjectIdentity{
        .volume_serial_number = metadata.value().volume_serial,
        .file_id = metadata.value().file_id,
    };
    quarantine_ = created.take_value();
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::after_quarantine_create)) {
      return failed_quarantine(
          injected_error(injection_.point),
          EfiDeleteMutationExtent::partial_or_unknown);
    }
    return EfiDeleteQuarantineCreateResult{
        .step = EfiDeletePlatformStepResult::completed(),
        .identity = quarantine_identity_.value(),
    };
  }

  EfiDeletePlatformStepResult move_candidate_to_quarantine_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      const std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity) override {
    const auto valid = validate_candidate(
        reviewed,
        candidate_index,
        candidate,
        quarantine_identity,
        CandidateLocation::original);
    if (!valid) {
      return failed_step(valid.error(), EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_candidate_move,
            candidate_index)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, false);
    }
    const auto tree = verify_candidate_tree_handle(
        candidate_roots_[candidate_index].get(), candidate);
    if (!tree) {
      return failed_step(tree.error(), EfiDeleteMutationExtent::none);
    }
    const auto slot =
        efi_delete_quarantine_slot_relative_path(candidate_index);
    if (!slot) {
      return failed_step(slot.error(), EfiDeleteMutationExtent::none);
    }
    const std::size_t separator = slot.value().rfind(L'\\');
    const std::wstring slot_name = slot.value().substr(separator + 1U);
    const auto renamed = rename_handle_no_replace(
        candidate_roots_[candidate_index].get(),
        quarantine_.get(),
        slot_name);
    if (!renamed) {
      return failed_step(renamed.error(), EfiDeleteMutationExtent::none);
    }
    locations_[candidate_index] = CandidateLocation::quarantined;
    auto readback = query_handle_name(candidate_roots_[candidate_index].get());
    const std::wstring expected_suffix =
        L"\\" + std::wstring(efi_delete_quarantine_namespace()) +
        L"\\" + slot_name;
    const auto moved_tree = verify_candidate_tree_handle(
        candidate_roots_[candidate_index].get(), candidate);
    if (!readback || !name_has_suffix(readback.value(), expected_suffix) ||
        !moved_tree) {
      return failed_step(
          !readback
              ? readback.error()
              : !moved_tree
                  ? moved_tree.error()
                  : windows_delete_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_REVISION_MISMATCH,
                        L"EFI quarantine rename readback",
                        L"同じcandidate handleのquarantine名を確認できません"),
          EfiDeleteMutationExtent::partial_or_unknown);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::after_candidate_move,
            candidate_index)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, true);
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  rollback_candidate_from_quarantine_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      const std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity) override {
    const auto valid = validate_candidate(
        reviewed,
        candidate_index,
        candidate,
        quarantine_identity,
        CandidateLocation::quarantined);
    if (!valid) {
      return failed_step(valid.error(), EfiDeleteMutationExtent::none);
    }
    const auto tree = verify_candidate_tree_handle(
        candidate_roots_[candidate_index].get(), candidate);
    if (!tree) {
      return failed_step(tree.error(), EfiDeleteMutationExtent::none);
    }
    const auto renamed = rename_handle_no_replace(
        candidate_roots_[candidate_index].get(),
        efi_root_.get(),
        candidate.relative_name);
    if (!renamed) {
      return failed_step(renamed.error(), EfiDeleteMutationExtent::none);
    }
    locations_[candidate_index] = CandidateLocation::original;
    auto readback = query_handle_name(candidate_roots_[candidate_index].get());
    const auto restored_tree = verify_candidate_tree_handle(
        candidate_roots_[candidate_index].get(), candidate);
    const std::wstring expected_suffix =
        L"\\EFI\\" + candidate.relative_name;
    if (!readback || !name_has_suffix(readback.value(), expected_suffix) ||
        !restored_tree) {
      return failed_step(
          !readback
              ? readback.error()
              : !restored_tree
                  ? restored_tree.error()
                  : windows_delete_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_REVISION_MISMATCH,
                        L"EFI rollback rename readback",
                        L"同じcandidate handleの元名復帰を確認できません"),
          EfiDeleteMutationExtent::partial_or_unknown);
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult rebuild_microsoft_bcd_and_verify_readback(
      const ReviewedEfiDeletePlan& reviewed) override {
    const auto valid = validate_review(reviewed);
    if (!valid) {
      return failed_step(valid.error(), EfiDeleteMutationExtent::none);
    }
    if (std::any_of(
            locations_.begin(), locations_.end(),
            [](const CandidateLocation location) {
              return location != CandidateLocation::quarantined;
            })) {
      return failed_step(
          windows_delete_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"EFI delete BCD ordering",
              L"全candidateのquarantine完了前にBCDを再構築できません"),
          EfiDeleteMutationExtent::none);
    }
    const auto mapping = verify_system_root_maps_to_esp(
        requests_.front().target_system_partition_root,
        reviewed.expected_esp());
    if (!mapping) {
      return failed_step(mapping.error(), EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_bcd_rebuild)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, false);
    }
    auto file_system = make_windows_bcd_store_file_system();
    auto trust = make_windows_authenticode_verifier();
    auto runner = make_windows_process_runner();
    auto system_directory = trusted_system_directory();
    auto store = bcd_store_path(requests_.front());
    if (!file_system || !trust || !runner || !system_directory || !store) {
      const clonecore::Error error = !system_directory
          ? system_directory.error()
          : !store
              ? store.error()
              : windows_delete_error(
                    clonecore::ErrorCode::internal_error,
                    ERROR_NOT_ENOUGH_MEMORY,
                    L"EFI delete BCD dependencies",
                    L"BCD transaction dependencyを作成できませんでした");
      return failed_step(error, EfiDeleteMutationExtent::none);
    }
    store_path_ = store.value();
    outer_backup_path_ = store_path_ + std::wstring(kBcdRollbackSuffix);
    const auto foreign_backup =
        file_system->observe_regular_file_identity(outer_backup_path_);
    const auto prior = file_system->observe_regular_file_identity(store_path_);
    if (!foreign_backup || !prior) {
      return failed_step(
          !foreign_backup ? foreign_backup.error() : prior.error(),
          EfiDeleteMutationExtent::none);
    }
    if (foreign_backup.value().has_value()) {
      return EfiDeletePlatformStepResult::failed(
          EfiDeletePlatformFailureKind::foreign_object,
          EfiDeleteMutationExtent::none,
          windows_delete_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ALREADY_EXISTS,
              L"EFI delete outer BCD backup",
              L"固定owned BCD rollback名に既存objectがあるため開始しません"));
    }
    prior_bcd_identity_ = prior.value();
    if (prior_bcd_identity_.has_value()) {
      const auto moved = file_system->move_file_no_replace(
          store_path_, outer_backup_path_, prior_bcd_identity_.value());
      if (!moved) {
        const auto source_after_failure =
            file_system->observe_regular_file_identity(store_path_);
        const auto destination_after_failure =
            file_system->observe_regular_file_identity(outer_backup_path_);
        const EfiDeleteMutationExtent extent =
            source_after_failure && destination_after_failure
            ? classify_windows_efi_delete_failed_bcd_backup_move_readback(
                  prior_bcd_identity_.value(),
                  source_after_failure.value(),
                  destination_after_failure.value())
            : EfiDeleteMutationExtent::partial_or_unknown;
        return failed_step(moved.error(), extent);
      }
      bcd_prior_moved_ = true;
      const auto source_after_move =
          file_system->observe_regular_file_identity(store_path_);
      const auto destination_after_move =
          file_system->observe_regular_file_identity(outer_backup_path_);
      if (!source_after_move || source_after_move.value().has_value() ||
          !destination_after_move ||
          !destination_after_move.value().has_value() ||
          !same_bcd_store_object(
              prior_bcd_identity_.value(),
              destination_after_move.value().value())) {
        return failed_step(
            !source_after_move
                ? source_after_move.error()
                : !destination_after_move
                    ? destination_after_move.error()
                    : windows_delete_error(
                          clonecore::ErrorCode::verification_failed,
                          ERROR_REVISION_MISMATCH,
                          L"EFI delete outer BCD backup readback",
                          L"source不在と同一file objectのbackup identityを確認できません"),
            EfiDeleteMutationExtent::partial_or_unknown);
      }
      // Rename may legitimately change FILE_BASIC_INFO::ChangeTime. Keep the
      // exact post-rename identity for the later rollback or commit handle.
      prior_bcd_identity_ = destination_after_move.value().value();
      if (injection_matches(
              injection_,
              WindowsEfiDeleteFailurePoint::after_bcd_backup_move)) {
        return make_windows_efi_delete_injected_failure(
            injection_.point, true);
      }
    }

    auto report = execute_multi_windows_bcdboot_with_store_transaction(
        requests_,
        system_directory.value(),
        *trust,
        *runner,
        *file_system);
    if (!report) {
      const auto current =
          file_system->observe_regular_file_identity(store_path_);
      if (!current || current.value().has_value()) {
        return failed_step(
            !current ? current.error() : report.error(),
            EfiDeleteMutationExtent::partial_or_unknown);
      }
      if (bcd_prior_moved_) {
        const auto restored = file_system->move_file_no_replace(
            outer_backup_path_, store_path_, prior_bcd_identity_.value());
        if (!restored) {
          return failed_step(
              restored.error(), EfiDeleteMutationExtent::partial_or_unknown);
        }
        bcd_prior_moved_ = false;
      }
      prior_bcd_identity_.reset();
      return failed_step(report.error(), EfiDeleteMutationExtent::none);
    }
    const auto new_store =
        file_system->observe_regular_file_identity(store_path_);
    if (!new_store || !new_store.value().has_value()) {
      return failed_step(
          !new_store
              ? new_store.error()
              : windows_delete_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_FILE_NOT_FOUND,
                    L"EFI delete new BCD identity",
                    L"BCD readback後のexact file identityを保持できません"),
          EfiDeleteMutationExtent::partial_or_unknown);
    }
    new_bcd_identity_ = new_store.value().value();
    verified_bcd_report_ = report.take_value();
    bcd_active_ = true;
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::after_bcd_rebuild)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, true);
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  rollback_microsoft_bcd_rebuild_if_identity_matches(
      const ReviewedEfiDeletePlan& reviewed) override {
    const auto valid = validate_review(reviewed);
    if (!valid || !bcd_active_ || !new_bcd_identity_.has_value()) {
      return failed_step(
          valid
              ? windows_delete_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_INVALID_DATA,
                    L"EFI delete BCD rollback state",
                    L"retained BCD rollback boundaryがありません")
              : valid.error(),
          EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_bcd_rollback)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, false);
    }
    auto file_system = make_windows_bcd_store_file_system();
    if (!file_system) {
      return failed_step(
          windows_delete_error(
              clonecore::ErrorCode::internal_error,
              ERROR_NOT_ENOUGH_MEMORY,
              L"EFI delete BCD rollback filesystem",
              L"exact-identity filesystemを作成できませんでした"),
          EfiDeleteMutationExtent::none);
    }
    bool mutated = false;
    const auto removed = file_system->remove_file_if_identity_matches(
        store_path_, new_bcd_identity_.value());
    if (!removed) {
      return failed_step(removed.error(), EfiDeleteMutationExtent::none);
    }
    mutated = true;
    if (bcd_prior_moved_ && prior_bcd_identity_.has_value()) {
      const auto restored = file_system->move_file_no_replace(
          outer_backup_path_, store_path_, prior_bcd_identity_.value());
      if (!restored) {
        return failed_step(
            restored.error(), EfiDeleteMutationExtent::partial_or_unknown);
      }
    }
    const auto restored_store =
        file_system->observe_regular_file_identity(store_path_);
    const bool expected_present = prior_bcd_identity_.has_value();
    if (!restored_store ||
        restored_store.value().has_value() != expected_present ||
        (expected_present &&
         !equivalent_bcd_store_file_identity(
             restored_store.value().value(), prior_bcd_identity_.value()))) {
      return failed_step(
          !restored_store
              ? restored_store.error()
              : windows_delete_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_REVISION_MISMATCH,
                    L"EFI delete BCD rollback readback",
                    L"prior BCDのexact identity復帰を確認できません"),
          mutated ? EfiDeleteMutationExtent::partial_or_unknown
                  : EfiDeleteMutationExtent::none);
    }
    bcd_active_ = false;
    bcd_prior_moved_ = false;
    new_bcd_identity_.reset();
    prior_bcd_identity_.reset();
    verified_bcd_report_.reset();
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult commit_microsoft_bcd_rebuild(
      const ReviewedEfiDeletePlan& reviewed) override {
    const auto valid = validate_review(reviewed);
    if (!valid || !bcd_active_ || !new_bcd_identity_.has_value()) {
      return failed_step(
          valid
              ? windows_delete_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_INVALID_DATA,
                    L"EFI delete BCD commit state",
                    L"retained BCD rollback boundaryがありません")
              : valid.error(),
          EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_bcd_commit)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, false);
    }
    if (bcd_prior_moved_ && prior_bcd_identity_.has_value()) {
      auto file_system = make_windows_bcd_store_file_system();
      if (!file_system) {
        return failed_step(
            windows_delete_error(
                clonecore::ErrorCode::internal_error,
                ERROR_NOT_ENOUGH_MEMORY,
                L"EFI delete BCD commit filesystem",
                L"exact-identity filesystemを作成できませんでした"),
            EfiDeleteMutationExtent::none);
      }
      const auto removed = file_system->remove_file_if_identity_matches(
          outer_backup_path_, prior_bcd_identity_.value());
      if (!removed) {
        return failed_step(
            removed.error(), EfiDeleteMutationExtent::partial_or_unknown);
      }
    }
    bcd_active_ = false;
    bcd_prior_moved_ = false;
    prior_bcd_identity_.reset();
    new_bcd_identity_.reset();
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  delete_quarantined_candidate_tree_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      const std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity) override {
    const auto valid = validate_candidate(
        reviewed,
        candidate_index,
        candidate,
        quarantine_identity,
        CandidateLocation::quarantined);
    if (!valid) {
      return failed_step(valid.error(), EfiDeleteMutationExtent::none);
    }
    const auto tree = verify_candidate_tree_handle(
        candidate_roots_[candidate_index].get(), candidate);
    if (!tree) {
      return failed_step(tree.error(), EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_candidate_delete,
            candidate_index)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, false);
    }
    const auto removed = recursively_delete_manifest_tree(
        candidate_roots_[candidate_index].get(),
        candidate,
        candidate.relative_name,
        true);
    if (!removed) {
      return failed_step(
          removed.error(), EfiDeleteMutationExtent::partial_or_unknown);
    }
    candidate_roots_[candidate_index].reset();
    locations_[candidate_index] = CandidateLocation::deleted;
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::after_candidate_delete,
            candidate_index)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, true);
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  remove_owned_quarantine_if_empty_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      const EfiDeleteObjectIdentity& quarantine_identity) override {
    const auto valid = validate_review(reviewed);
    if (!valid || !same_quarantine_identity(quarantine_identity) ||
        std::any_of(
            locations_.begin(), locations_.end(),
            [](const CandidateLocation location) {
              return location != CandidateLocation::deleted;
            })) {
      return failed_step(
          valid
              ? windows_delete_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_INVALID_DATA,
                    L"EFI quarantine cleanup state",
                    L"owned quarantine identityまたはcandidate deletion stateが一致しません")
              : valid.error(),
          EfiDeleteMutationExtent::none);
    }
    auto entries = enumerate_directory(quarantine_.get());
    if (!entries || !entries.value().empty()) {
      return failed_step(
          !entries
              ? entries.error()
              : windows_delete_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_DIR_NOT_EMPTY,
                    L"EFI quarantine final empty proof",
                    L"owned quarantineが空ではないため削除しません"),
          EfiDeleteMutationExtent::none);
    }
    if (injection_matches(
            injection_,
            WindowsEfiDeleteFailurePoint::before_quarantine_cleanup)) {
      return make_windows_efi_delete_injected_failure(
          injection_.point, false);
    }
    const auto marked = mark_handle_for_delete(quarantine_.get());
    if (!marked) {
      return failed_step(marked.error(), EfiDeleteMutationExtent::none);
    }
    quarantine_.reset();
    auto root_entries = enumerate_directory(volume_root_.get());
    if (!root_entries ||
        std::any_of(
            root_entries.value().begin(), root_entries.value().end(),
            [](const DirectoryEntry& entry) {
              return same_text(
                  entry.name, efi_delete_quarantine_namespace());
            })) {
      return failed_step(
          !root_entries
              ? root_entries.error()
              : windows_delete_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_ALREADY_EXISTS,
                    L"EFI quarantine removal readback",
                    L"owned quarantine handle deletionの不在確認に失敗しました"),
          EfiDeleteMutationExtent::partial_or_unknown);
    }
    quarantine_identity_.reset();
    return EfiDeletePlatformStepResult::completed();
  }

  const std::optional<MultiWindowsBcdBootReport>& verified_bcd_report()
      const noexcept override {
    return verified_bcd_report_;
  }

 private:
  clonecore::Status validate_review(
      const ReviewedEfiDeletePlan& reviewed) const {
    if (!retained_review_.has_value() || !volume_root_ || !efi_root_ ||
        candidate_roots_.size() != reviewed.candidates().size() ||
        !equivalent_efi_delete_manifest(
            retained_review_.value(), reviewed)) {
      return clonecore::Status::failure(windows_delete_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"EFI transaction retained review",
          L"fresh inspectionで保持したexact manifestとtransaction planが一致しません"));
    }
    return clonecore::success_status();
  }

  bool same_quarantine_identity(
      const EfiDeleteObjectIdentity& identity) const noexcept {
    return quarantine_ && quarantine_identity_.has_value() &&
        quarantine_identity_->volume_serial_number ==
            identity.volume_serial_number &&
        quarantine_identity_->file_id == identity.file_id;
  }

  clonecore::Status validate_candidate(
      const ReviewedEfiDeletePlan& reviewed,
      const std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity,
      const CandidateLocation expected_location) const {
    const auto valid = validate_review(reviewed);
    if (!valid) {
      return valid;
    }
    if (!same_quarantine_identity(quarantine_identity) ||
        candidate_index >= candidate_roots_.size() ||
        !candidate_roots_[candidate_index] ||
        locations_[candidate_index] != expected_location ||
        !same_text(
            reviewed.candidates()[candidate_index].relative_name,
            candidate.relative_name) ||
        reviewed.candidates()[candidate_index].entries.size() !=
            candidate.entries.size()) {
      return clonecore::Status::failure(windows_delete_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"EFI transaction candidate identity",
          L"candidate index、manifest、retained handle、quarantine identityが一致しません"));
    }
    return clonecore::success_status();
  }

  static EfiDeleteQuarantineCreateResult failed_quarantine(
      const clonecore::Error& error,
      const EfiDeleteMutationExtent extent) {
    return EfiDeleteQuarantineCreateResult{
        .step = failed_step(error, extent),
        .identity = {},
    };
  }

  std::vector<BcdBootRequest> requests_;
  WindowsEfiDeleteFailureInjection injection_;
  std::optional<ReviewedEfiDeletePlan> retained_review_;
  clonecore::UniqueHandle volume_root_;
  clonecore::UniqueHandle efi_root_;
  std::vector<clonecore::UniqueHandle> candidate_roots_;
  std::vector<CandidateLocation> locations_;
  clonecore::UniqueHandle quarantine_;
  std::optional<EfiDeleteObjectIdentity> quarantine_identity_;
  std::wstring store_path_;
  std::wstring outer_backup_path_;
  std::optional<BcdStoreFileIdentity> prior_bcd_identity_;
  std::optional<BcdStoreFileIdentity> new_bcd_identity_;
  std::optional<MultiWindowsBcdBootReport> verified_bcd_report_;
  bool bcd_prior_moved_{};
  bool bcd_active_{};
};

clonecore::Status validate_bcd_requests(
    const std::vector<BcdBootRequest>& requests) {
  if (requests.empty() || requests.size() > 32U) {
    return clonecore::Status::failure(windows_delete_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"EFI delete BCDBoot request count",
        L"1件以上32件以下のWindows登録requestが必要です"));
  }
  const std::wstring system_root =
      normalize_root(requests.front().target_system_partition_root);
  for (std::size_t index = 0U; index < requests.size(); ++index) {
    const auto arguments = build_bcdboot_arguments(requests[index]);
    if (!arguments) {
      return clonecore::Status::failure(arguments.error());
    }
    if (requests[index].firmware != BcdBootFirmware::uefi ||
        !same_text(
            normalize_root(requests[index].target_system_partition_root),
            system_root) ||
        requests[index].store_policy !=
            (index == 0U
                 ? BcdBootStorePolicy::rebuild_fresh
                 : BcdBootStorePolicy::preserve_existing)) {
      return clonecore::Status::failure(windows_delete_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"EFI delete BCDBoot request batch",
          L"UEFI、同一/s root、初回rebuild、後続preserveのbatchだけを受理します"));
    }
  }
  return clonecore::success_status();
}

}  // namespace

EfiDeleteMutationExtent
classify_windows_efi_delete_failed_bcd_backup_move_readback(
    const BcdStoreFileIdentity& expected_source,
    const std::optional<BcdStoreFileIdentity>& observed_source,
    const std::optional<BcdStoreFileIdentity>& observed_destination) noexcept {
  return observed_source.has_value() &&
          equivalent_bcd_store_file_identity(
              expected_source, observed_source.value()) &&
          !observed_destination.has_value()
      ? EfiDeleteMutationExtent::none
      : EfiDeleteMutationExtent::partial_or_unknown;
}

clonecore::Result<EfiDeleteEspIdentity>
inspect_windows_efi_delete_esp_identity_read_only(
    const WindowsEfiDeleteEspRequest& request) {
  auto observed = observe_windows_esp_read_only(request);
  if (!observed) {
    return clonecore::Result<EfiDeleteEspIdentity>::failure(
        observed.error());
  }
  return clonecore::Result<EfiDeleteEspIdentity>::success(
      std::move(observed.value().esp));
}

std::unique_ptr<IEfiDeleteReadOnlyInspector>
make_windows_efi_delete_read_only_inspector() {
  return std::make_unique<WindowsEfiDeleteReadOnlyInspector>();
}

EfiDeletePlatformStepResult make_windows_efi_delete_injected_failure(
    const WindowsEfiDeleteFailurePoint point,
    const bool mutation_may_have_occurred) {
  if (point == WindowsEfiDeleteFailurePoint::none) {
    return EfiDeletePlatformStepResult::failed(
        EfiDeletePlatformFailureKind::platform_contract_violation,
        EfiDeleteMutationExtent::none,
        windows_delete_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"EFI delete failure injection",
            L"noneはfailure injectionとして指定できません"));
  }
  return EfiDeletePlatformStepResult::failed(
      EfiDeletePlatformFailureKind::io_failure,
      mutation_may_have_occurred
          ? EfiDeleteMutationExtent::partial_or_unknown
          : EfiDeleteMutationExtent::none,
      injected_error(point));
}

clonecore::Result<std::unique_ptr<IWindowsEfiDeleteTransactionPlatform>>
make_windows_efi_delete_transaction_platform_for_failure_injection(
    std::vector<BcdBootRequest> requests_in_boot_priority,
    const WindowsEfiDeleteFailureInjection injection) {
  const auto valid = validate_bcd_requests(requests_in_boot_priority);
  if (!valid) {
    return clonecore::Result<
        std::unique_ptr<IWindowsEfiDeleteTransactionPlatform>>::failure(
        valid.error());
  }
  return clonecore::Result<
      std::unique_ptr<IWindowsEfiDeleteTransactionPlatform>>::success(
      std::make_unique<WindowsEfiDeleteTransactionPlatform>(
          std::move(requests_in_boot_priority), injection));
}

clonecore::Result<std::unique_ptr<IWindowsEfiDeleteTransactionPlatform>>
make_windows_efi_delete_transaction_platform(
    std::vector<BcdBootRequest> requests_in_boot_priority) {
  return make_windows_efi_delete_transaction_platform_for_failure_injection(
      std::move(requests_in_boot_priority), {});
}

}  // namespace ytec::bootrepair

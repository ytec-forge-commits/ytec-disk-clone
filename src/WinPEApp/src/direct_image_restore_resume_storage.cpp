#include "ytec/winpeapp/direct_image_restore_resume_storage.h"

#include "ytec/winpeapp/direct_image_create_resume.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/winpeapp/active_rescue_media.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;

clonecore::Error storage_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return {
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
  return clonecore::Result<T>::failure(storage_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

void append_u32(
    std::vector<std::byte>& bytes,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(
    std::vector<std::byte>& bytes,
    const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_domain(
    std::vector<std::byte>& bytes,
    const std::string_view domain) {
  append_u32(bytes, static_cast<std::uint32_t>(domain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
}

void append_wstring(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    const auto unit = static_cast<std::uint16_t>(character);
    bytes.push_back(static_cast<std::byte>(unit & 0xffU));
    bytes.push_back(static_cast<std::byte>((unit >> 8U) & 0xffU));
  }
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      left.size() <=
          static_cast<std::size_t>((std::numeric_limits<int>::max)()) &&
      CompareStringOrdinal(
          left.data(),
          static_cast<int>(left.size()),
          right.data(),
          static_cast<int>(right.size()),
          TRUE) == CSTR_EQUAL;
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& path) {
  if (path.size() < 3U || path.size() >= kMaximumPathCharacters ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'/') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      path.find(L'\0') != std::wstring::npos || path.ends_with(L" ") ||
      path.ends_with(L".")) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"PE Resume storage path",
        L"正規化済みのローカル絶対パスが必要です");
  }
  std::vector<wchar_t> resolved(kMaximumPathCharacters, L'\0');
  wchar_t* file_part{};
  const DWORD length = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(resolved.size()),
      resolved.data(),
      &file_part);
  if (length == 0U || length >= resolved.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume storage canonical path",
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  const std::wstring canonical(resolved.data(), length);
  if (!equals_ordinal_ignore_case(path, canonical)) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"PE Resume storage canonical path",
        L"相対要素または正規化差分を含むパスは使用できません");
  }
  return clonecore::Result<std::wstring>::success(canonical);
}

clonecore::Status verify_final_dos_path(
    const HANDLE handle,
    const std::wstring& canonical) {
  std::vector<wchar_t> actual(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle,
      actual.data(),
      static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0U || length >= actual.size()) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::identity_mismatch,
        L"PE Resume opened path",
        length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  const std::wstring expected = L"\\\\?\\" + canonical;
  if (!equals_ordinal_ignore_case(
          std::wstring_view(actual.data(), length), expected)) {
    return clonecore::Status::failure(storage_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        L"PE Resume opened path",
        L"opened handleの実体パスが確定済みパスと一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::wstring> opened_volume_guid(
    const HANDLE handle) {
  std::vector<wchar_t> actual(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(
      handle,
      actual.data(),
      static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
  if (length == 0U || length >= actual.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume opened volume GUID",
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  const std::wstring_view path(actual.data(), length);
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (!path.starts_with(prefix)) {
    return failure<std::wstring>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_INVALID_NAME,
        L"PE Resume opened volume GUID",
        L"opened handleからVolume GUIDを取得できません");
  }
  const std::size_t close = path.find(L"}\\", prefix.size());
  if (close == std::wstring_view::npos) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"PE Resume opened volume GUID",
        L"Volume GUID形式が不正です");
  }
  std::wstring volume(path.substr(0U, close + 2U));
  std::transform(
      volume.begin(), volume.end(), volume.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towupper(value));
      });
  return clonecore::Result<std::wstring>::success(std::move(volume));
}

clonecore::Result<std::wstring> mounted_volume_guid_for_path(
    const std::wstring& canonical) {
  std::vector<wchar_t> root(kMaximumPathCharacters, L'\0');
  if (!GetVolumePathNameW(
          canonical.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume mounted volume root",
            GetLastError()));
  }
  std::vector<wchar_t> volume(kMaximumPathCharacters, L'\0');
  if (!GetVolumeNameForVolumeMountPointW(
          root.data(), volume.data(), static_cast<DWORD>(volume.size()))) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume mounted volume GUID",
            GetLastError()));
  }
  std::wstring result(volume.data());
  if (!result.starts_with(L"\\\\?\\Volume{") ||
      !result.ends_with(L"}\\")) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"PE Resume mounted volume GUID",
        L"mount pointから正規Volume GUIDを取得できません");
  }
  std::transform(
      result.begin(), result.end(), result.begin(), [](const wchar_t value) {
        return static_cast<wchar_t>(std::towupper(value));
      });
  return clonecore::Result<std::wstring>::success(std::move(result));
}

clonecore::Result<OpenedResumeStorageDomainV1> hash_volume_domain(
    const HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION basic{};
  if (!GetFileInformationByHandle(handle, &basic) ||
      basic.dwVolumeSerialNumber == 0U) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume opened volume serial",
            GetLastError()));
  }
  auto volume = opened_volume_guid(handle);
  if (!volume) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        volume.error());
  }
  std::vector<std::byte> material;
  material.reserve(128U);
  append_domain(material, "YTEC-PE-OPENED-VOLUME-STORAGE-DOMAIN-V1");
  append_u32(material, basic.dwVolumeSerialNumber);
  append_wstring(material, volume.value());
  auto digest = imageformat::sha256(material);
  if (!digest || all_zero(digest.value())) {
    return digest
        ? failure<OpenedResumeStorageDomainV1>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"PE Resume opened volume domain",
              L"非ゼロのstorage domain Hashを作成できません")
        : clonecore::Result<OpenedResumeStorageDomainV1>::failure(
              digest.error());
  }
  return clonecore::Result<OpenedResumeStorageDomainV1>::success({
      .storage_identity_hash = digest.take_value(),
      .identity_from_open_handles = true,
      .persistent_checkpoint_backing = false,
  });
}

clonecore::Result<operationcore::Sha256Digest>
hash_opened_image_file_object(const HANDLE handle) {
  FILE_ID_INFO identifier{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          handle, FileIdInfo, &identifier, sizeof(identifier)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic)) ||
      identifier.VolumeSerialNumber == 0U ||
      standard.EndOfFile.QuadPart <= 0) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume selected image file object",
            GetLastError()));
  }
  std::array<std::byte, 16U> file_id{};
  static_assert(sizeof(identifier.FileId.Identifier) == file_id.size());
  std::memcpy(
      file_id.data(), identifier.FileId.Identifier, file_id.size());
  if (all_zero(file_id)) {
    return failure<operationcore::Sha256Digest>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE Resume selected image file object",
        L"opened imageのFile IDを取得できません");
  }
  const auto last_write =
      static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
  if (last_write == 0U) {
    return failure<operationcore::Sha256Digest>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE Resume selected image file object",
        L"opened imageの更新時刻を取得できません");
  }
  std::vector<std::byte> material;
  material.reserve(96U);
  append_domain(material, "YTEC-PE-RESUME-IMAGE-FILE-OBJECT-V1");
  append_u64(material, identifier.VolumeSerialNumber);
  append_array(material, file_id);
  append_u64(
      material, static_cast<std::uint64_t>(standard.EndOfFile.QuadPart));
  append_u64(material, last_write);
  return imageformat::sha256(material);
}

clonecore::Result<clonecore::StableDiskIdentity> identity_for_disk_number(
    const std::uint32_t disk_number) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (!inventory) {
    return failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"PE Resume storage inventory",
        L"ディスク一覧プロバイダーを作成できません");
  }
  auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"PE Resume storage inventory",
        L"ディスク列挙に未解決の診断があるためstorage domainを確定できません");
  }
  const auto matches = [&](const diskmodel::DiskInfo& disk) {
    return disk.disk_number == disk_number;
  };
  if (std::count_if(
          report.value().disks.begin(),
          report.value().disks.end(),
          matches) != 1) {
    return failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE Resume storage disk number",
        L"opened volumeを物理ディスク一覧へ一意に対応付けできません");
  }
  const auto found = std::find_if(
      report.value().disks.begin(), report.value().disks.end(), matches);
  return diskmodel::make_stable_disk_identity(
      *found, found->is_system_disk);
}

clonecore::Result<OpenedResumeStorageDomainV1> open_and_hash_physical(
    const clonecore::StableDiskIdentity& identity) {
  auto opened = diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
      identity);
  if (!opened) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        opened.error());
  }
  const auto same = clonecore::validate_stable_identity(
      identity, opened.value().observed.identity, L"PE Resume opened storage");
  if (!same || !opened.value().reader ||
      opened.value().reader->size_bytes() != identity.size_bytes ||
      opened.value().reader->logical_sector_size() !=
          identity.logical_sector_size) {
    return same
        ? failure<OpenedResumeStorageDomainV1>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"PE Resume opened storage geometry",
              L"opened raw diskの容量またはsectorが安定識別と一致しません")
        : clonecore::Result<OpenedResumeStorageDomainV1>::failure(
              same.error());
  }
  auto digest = imageformat::hash_tsumugi_physical_restore_target_identity_v1(
      opened.value().observed.identity);
  if (!digest || all_zero(digest.value())) {
    return digest
        ? failure<OpenedResumeStorageDomainV1>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"PE Resume opened physical domain",
              L"非ゼロのphysical storage Hashを作成できません")
        : clonecore::Result<OpenedResumeStorageDomainV1>::failure(
              digest.error());
  }
  return clonecore::Result<OpenedResumeStorageDomainV1>::success({
      .storage_identity_hash = digest.take_value(),
      .identity_from_open_handles = true,
  });
}

struct StorageBindingState final {
  std::mutex mutex;
  std::optional<DirectImageRestoreResumeStorageProof> selected;
};

struct ImageCreateStorageBindingState final {
  std::mutex mutex;
  std::optional<DirectImageCreateResumeStorageProof> selected;
};

clonecore::Result<std::wstring> current_data_directory() {
  std::vector<wchar_t> module(kMaximumPathCharacters, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, module.data(), static_cast<DWORD>(module.size()));
  if (length == 0U || length >= module.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume executable path",
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  return clonecore::Result<std::wstring>::success(
      (std::filesystem::path(std::wstring(module.data(), length))
           .parent_path() /
       L"data")
          .wstring());
}

}  // namespace

clonecore::Result<DirectImageRestoreResumeStoragePlatformV1>
make_direct_image_restore_resume_storage_platform_v1(
    DirectImageRestoreResumeStorageDependenciesV1 dependencies) {
  if (!dependencies.observe_path_storage ||
      !dependencies.observe_locked_target_storage ||
      !dependencies.observe_active_rescue_storage) {
    return failure<DirectImageRestoreResumeStoragePlatformV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE Resume storage platform",
        L"path、locked target、またはactive rescueのopened-storage observerがありません");
  }
  auto state = std::make_shared<StorageBindingState>();
  auto path_observer = dependencies.observe_path_storage;
  return clonecore::Result<
      DirectImageRestoreResumeStoragePlatformV1>::success({
      .prove_data_backing =
          [state, path_observer](
              const std::wstring& data_directory,
              const std::optional<operationcore::ResumeSlotRecord>&) {
            auto data = path_observer(
                data_directory, ResumeStoragePathRole::checkpoint_data);
            if (!data) {
              return clonecore::Result<
                  operationcore::WindowsResumeDataBackingProof>::failure(
                  data.error());
            }
            bool separated = false;
            {
              std::scoped_lock lock(state->mutex);
              if (state->selected) {
                separated =
                    data.value().persistent_checkpoint_backing &&
                    data.value().storage_identity_hash ==
                        state->selected->checkpoint_storage_identity_hash &&
                    data.value().storage_identity_hash !=
                        state->selected->image_storage_identity_hash &&
                    data.value().storage_identity_hash !=
                        state->selected->target_storage_identity_hash;
              }
            }
            return clonecore::Result<
                operationcore::WindowsResumeDataBackingProof>::success({
                .backing_storage_identity_hash =
                    data.value().storage_identity_hash,
                .identity_from_open_handle =
                    data.value().identity_from_open_handles,
                .separated_from_source = separated,
            });
          },
      .prove_restore_storage =
          [state, dependencies = std::move(dependencies)](
              const DirectImageRestoreRequest& request) {
            auto data = dependencies.observe_path_storage(
                [&]() {
                  std::vector<wchar_t> module(kMaximumPathCharacters, L'\0');
                  const DWORD length = GetModuleFileNameW(
                      nullptr,
                      module.data(),
                      static_cast<DWORD>(module.size()));
                  if (length == 0U || length >= module.size()) {
                    return std::wstring{};
                  }
                  return (std::filesystem::path(
                              std::wstring(module.data(), length))
                              .parent_path() /
                          L"data")
                      .wstring();
                }(),
                ResumeStoragePathRole::checkpoint_data);
            if (!data) {
              return clonecore::Result<
                  DirectImageRestoreResumeStorageProof>::failure(
                  data.error());
            }
            if (!data.value().persistent_checkpoint_backing) {
              return failure<DirectImageRestoreResumeStorageProof>(
                  clonecore::ErrorCode::unsupported_layout,
                  ERROR_NOT_SUPPORTED,
                  L"PE Resume persistent checkpoint backing",
                  L"WinPE X:または読取り専用媒体上のdataは再起動後に保持されないため、image/target I/O前に停止しました");
            }
            auto image = dependencies.observe_path_storage(
                request.image.image_path, ResumeStoragePathRole::image);
            if (!image) {
              return clonecore::Result<
                  DirectImageRestoreResumeStorageProof>::failure(
                  image.error());
            }
            auto target = dependencies.observe_locked_target_storage(
                request.expected_target);
            if (!target) {
              return clonecore::Result<
                  DirectImageRestoreResumeStorageProof>::failure(
                  target.error());
            }
            auto active = dependencies.observe_active_rescue_storage();
            if (!active) {
              return clonecore::Result<
                  DirectImageRestoreResumeStorageProof>::failure(
                  active.error());
            }
            DirectImageRestoreResumeStorageProof proof{
                .checkpoint_storage_identity_hash =
                    data.value().storage_identity_hash,
                .image_storage_identity_hash =
                    image.value().storage_identity_hash,
                .target_storage_identity_hash =
                    target.value().storage_identity_hash,
                .active_rescue_storage_identity_hash =
                    active.value().storage_identity_hash,
                .image_file_object_identity_hash =
                    image.value().file_object_identity_hash.value_or(
                        operationcore::Sha256Digest{}),
                .all_identities_from_open_handles =
                    data.value().identity_from_open_handles &&
                    image.value().identity_from_open_handles &&
                    target.value().identity_from_open_handles &&
                    active.value().identity_from_open_handles,
            };
            {
              std::scoped_lock lock(state->mutex);
              state->selected = proof;
            }
            return clonecore::Result<
                DirectImageRestoreResumeStorageProof>::success(proof);
          },
  });
}

clonecore::Result<OpenedResumeStorageDomainV1>
observe_windows_resume_path_storage_domain_v1(
    const std::wstring& path,
    const ResumeStoragePathRole role) {
  auto canonical = canonical_local_path(path);
  if (!canonical) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        canonical.error());
  }
  const bool directory_role =
      role == ResumeStoragePathRole::checkpoint_data ||
      role == ResumeStoragePathRole::image_output_parent;
  const DWORD attributes = GetFileAttributesW(canonical.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      (!directory_role && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) ||
      (directory_role && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)) {
    return failure<OpenedResumeStorageDomainV1>(
        clonecore::ErrorCode::unsupported_layout,
        attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                               : ERROR_REPARSE_TAG_INVALID,
        L"PE Resume storage path attributes",
        L"通常の非reparse imageファイルまたはdataディレクトリだけを使用できます");
  }
  clonecore::UniqueHandle opened(CreateFileW(
      canonical.value().c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT |
          ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
               ? FILE_FLAG_BACKUP_SEMANTICS
               : 0U),
      nullptr));
  if (!opened) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Resume storage path open",
            GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(
      opened.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      (!directory_role &&
       (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) ||
      (directory_role &&
       (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)) {
    return failure<OpenedResumeStorageDomainV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"PE Resume opened storage attributes",
        L"opened handleが通常の非reparse対象ではありません");
  }
  const auto path_before = verify_final_dos_path(
      opened.get(), canonical.value());
  if (!path_before) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        path_before.error());
  }

  auto held_volume = opened_volume_guid(opened.get());
  auto mounted_before = mounted_volume_guid_for_path(canonical.value());
  if (!held_volume) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        held_volume.error());
  }
  if (!mounted_before) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        mounted_before.error());
  }
  if (!equals_ordinal_ignore_case(
          held_volume.value(), mounted_before.value())) {
    return failure<OpenedResumeStorageDomainV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE Resume opened/mounted volume binding",
        L"opened pathのVolume GUIDと現在のmount pointが一致しません");
  }

  auto disk_number = diskmodel::query_single_disk_number_for_local_path(
      canonical.value());
  clonecore::Result<OpenedResumeStorageDomainV1> domain = [&]() {
    if (disk_number) {
      auto identity = identity_for_disk_number(disk_number.value());
      return identity
          ? open_and_hash_physical(identity.value())
          : clonecore::Result<OpenedResumeStorageDomainV1>::failure(
                identity.error());
    }
    const std::wstring root = canonical.value().substr(0U, 3U);
    const UINT drive_type = GetDriveTypeW(root.c_str());
    const bool winpe_ram_data =
        role == ResumeStoragePathRole::checkpoint_data &&
        (canonical.value()[0] == L'X' || canonical.value()[0] == L'x');
    if (drive_type != DRIVE_CDROM && !winpe_ram_data) {
      return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
          disk_number.error());
    }
    return hash_volume_domain(opened.get());
  }();
  if (!domain) {
    return domain;
  }
  if (role == ResumeStoragePathRole::image) {
    auto file_object = hash_opened_image_file_object(opened.get());
    if (!file_object) {
      return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
          file_object.error());
    }
    domain.value().file_object_identity_hash = file_object.take_value();
  }
  const auto path_after = verify_final_dos_path(
      opened.get(), canonical.value());
  if (!path_after) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        path_after.error());
  }
  auto mounted_after = mounted_volume_guid_for_path(canonical.value());
  if (!mounted_after) {
    return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
        mounted_after.error());
  }
  if (!equals_ordinal_ignore_case(
          held_volume.value(), mounted_after.value())) {
    return failure<OpenedResumeStorageDomainV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE Resume opened/mounted volume recheck",
        L"storage domain照会中にmount pointのVolume GUIDが変化しました");
  }
  return domain;
}

clonecore::Result<OpenedResumeStorageDomainV1>
observe_windows_resume_physical_storage_domain_v1(
    const clonecore::StableDiskIdentity& identity) {
  return open_and_hash_physical(identity);
}

clonecore::Result<DirectImageRestoreResumeStoragePlatformV1>
make_direct_image_restore_windows_storage_platform_v1() {
  return make_direct_image_restore_resume_storage_platform_v1({
      .observe_path_storage =
          observe_windows_resume_path_storage_domain_v1,
      .observe_locked_target_storage =
          observe_windows_resume_physical_storage_domain_v1,
      .observe_active_rescue_storage = []() {
        auto active = query_active_rescue_media_storage_with_windows_apis();
        if (!active) {
          return clonecore::Result<OpenedResumeStorageDomainV1>::failure(
              active.error());
        }
        if (!active.value().marker_identity_from_open_handle) {
          return failure<OpenedResumeStorageDomainV1>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"PE Resume active rescue marker",
              L"active rescue markerをopened handleから識別できません");
        }
        auto domain = active.value().physical_identity
            ? observe_windows_resume_physical_storage_domain_v1(
                  *active.value().physical_identity)
            : observe_windows_resume_path_storage_domain_v1(
                  active.value().marker_path,
                  ResumeStoragePathRole::active_rescue);
        if (!domain) {
          return domain;
        }
        auto rechecked =
            query_active_rescue_media_storage_with_windows_apis();
        if (!rechecked ||
            !rechecked.value().marker_identity_from_open_handle ||
            rechecked.value().drive_type != active.value().drive_type ||
            !equals_ordinal_ignore_case(
                rechecked.value().marker_path,
                active.value().marker_path) ||
            rechecked.value().physical_identity.has_value() !=
                active.value().physical_identity.has_value()) {
          if (!rechecked) {
            return clonecore::Result<
                OpenedResumeStorageDomainV1>::failure(
                rechecked.error());
          }
          return failure<OpenedResumeStorageDomainV1>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"PE Resume active rescue storage recheck",
              L"opened storage domain照会中にactive rescue markerまたはmedia classが変化しました");
        }
        if (active.value().physical_identity) {
          const auto same = clonecore::validate_stable_identity(
              *active.value().physical_identity,
              *rechecked.value().physical_identity,
              L"PE Resume active rescue physical storage recheck");
          if (!same) {
            return clonecore::Result<
                OpenedResumeStorageDomainV1>::failure(same.error());
          }
        }
        return domain;
      },
  });
}

clonecore::Result<DirectImageCreateResumeStoragePlatformV1>
make_direct_image_create_windows_resume_storage_platform_v1() {
  auto state = std::make_shared<ImageCreateStorageBindingState>();
  operationcore::WindowsResumeDataBackingProbe backing =
      [state](
          const std::wstring& data_directory,
          const std::optional<operationcore::ResumeSlotRecord>&) {
        auto data = observe_windows_resume_path_storage_domain_v1(
            data_directory, ResumeStoragePathRole::checkpoint_data);
        if (!data) {
          return clonecore::Result<
              operationcore::WindowsResumeDataBackingProof>::failure(
              data.error());
        }
        bool separated = false;
        {
          std::scoped_lock lock(state->mutex);
          if (state->selected) {
            separated = data.value().persistent_checkpoint_backing &&
                data.value().storage_identity_hash ==
                    state->selected->checkpoint_storage_identity_hash &&
                data.value().storage_identity_hash !=
                    state->selected->source_storage_identity_hash &&
                data.value().storage_identity_hash !=
                    state->selected->destination_storage_identity_hash;
          }
        }
        return clonecore::Result<
            operationcore::WindowsResumeDataBackingProof>::success({
            .backing_storage_identity_hash =
                data.value().storage_identity_hash,
            .identity_from_open_handle =
                data.value().identity_from_open_handles,
            .separated_from_source = separated,
        });
      };
  DirectImageCreateResumeStorageProbe storage =
      [state](
          const DirectImageCreateRequest& request,
          const std::optional<operationcore::ResumeSlotRecord>&) {
        auto data_path = current_data_directory();
        if (!data_path) {
          return clonecore::Result<
              DirectImageCreateResumeStorageProof>::failure(
              data_path.error());
        }
        auto data = observe_windows_resume_path_storage_domain_v1(
            data_path.value(), ResumeStoragePathRole::checkpoint_data);
        auto source_identity = diskmodel::make_stable_disk_identity(
            request.selected_source,
            request.selected_source.is_system_disk);
        if (!data || !source_identity) {
          return clonecore::Result<
              DirectImageCreateResumeStorageProof>::failure(
              data ? source_identity.error() : data.error());
        }
        auto source = observe_windows_resume_physical_storage_domain_v1(
            source_identity.value());
        if (!source) {
          return clonecore::Result<
              DirectImageCreateResumeStorageProof>::failure(
              source.error());
        }
        const std::filesystem::path output(request.final_path);
        if (!output.has_parent_path() || output.parent_path().empty()) {
          return failure<DirectImageCreateResumeStorageProof>(
              clonecore::ErrorCode::invalid_argument,
              ERROR_BAD_PATHNAME,
              L"PE image-create output parent",
              L"完成名を含む既存の絶対保存先ディレクトリが必要です");
        }
        auto destination = observe_windows_resume_path_storage_domain_v1(
            output.parent_path().wstring(),
            ResumeStoragePathRole::image_output_parent);
        if (!destination) {
          return clonecore::Result<
              DirectImageCreateResumeStorageProof>::failure(
              destination.error());
        }
        DirectImageCreateResumeStorageProof proof{
            .checkpoint_storage_identity_hash =
                data.value().storage_identity_hash,
            .source_storage_identity_hash =
                source.value().storage_identity_hash,
            .destination_storage_identity_hash =
                destination.value().storage_identity_hash,
            .all_identities_from_open_handles =
                data.value().identity_from_open_handles &&
                source.value().identity_from_open_handles &&
                destination.value().identity_from_open_handles,
        };
        if (!data.value().persistent_checkpoint_backing ||
            !proof.all_identities_from_open_handles ||
            all_zero(proof.checkpoint_storage_identity_hash) ||
            all_zero(proof.source_storage_identity_hash) ||
            all_zero(proof.destination_storage_identity_hash) ||
            proof.checkpoint_storage_identity_hash ==
                proof.source_storage_identity_hash ||
            proof.checkpoint_storage_identity_hash ==
                proof.destination_storage_identity_hash ||
            proof.source_storage_identity_hash ==
                proof.destination_storage_identity_hash) {
          return failure<DirectImageCreateResumeStorageProof>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"PE image-create opened storage separation",
              L"永続data、read-only Source、保存先を3つの別opened-storage domainとして証明できません");
        }
        {
          std::scoped_lock lock(state->mutex);
          state->selected = proof;
        }
        return clonecore::Result<
            DirectImageCreateResumeStorageProof>::success(proof);
      };
  DirectImageCreateResumeSlotPlatformFactory factory =
      [backing](std::vector<operationcore::WindowsResumeOwnedObject> objects) {
        return operationcore::
            make_current_executable_windows_resume_slot_platform(
                backing, std::nullopt, std::move(objects));
      };
  return clonecore::Result<
      DirectImageCreateResumeStoragePlatformV1>::success({
      .prove_data_backing = std::move(backing),
      .prove_image_create_storage = std::move(storage),
      .make_bound_slot_platform = std::move(factory),
  });
}

}  // namespace ytec::winpeapp

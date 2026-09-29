#ifdef _MSC_VER
// Windows SDK 10.0.26100 applies a value-incompatible SAL annotation to
// RegOpenKeyExW parameter 3. Suppress only that SDK declaration false positive.
#pragma warning(disable : 6553)
#endif

#include "ytec/windowsapp/windows_data_rescue_clone.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/usb_volume_mapping.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::wstring_view kRescueMarkerRelativePath =
    L"YtecDiskClone\\rescue-media-id.txt";
constexpr std::size_t kRescueMarkerBytes = 36U;

clonecore::Error rescue_error(
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
  return clonecore::Result<T>::failure(rescue_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool valid_drive_root(const std::wstring_view root) noexcept {
  return root.size() == 3U &&
         ((root[0] >= L'A' && root[0] <= L'Z') ||
          (root[0] >= L'a' && root[0] <= L'z')) &&
         root[1] == L':' && root[2] == L'\\';
}

std::wstring canonical_drive_root(std::wstring root) {
  if (!root.empty()) {
    root[0] = static_cast<wchar_t>(std::towupper(root[0]));
  }
  return root;
}

bool valid_rescue_marker(const std::string_view marker) noexcept {
  if (marker.size() != kRescueMarkerBytes) {
    return false;
  }
  bool has_nonzero_hex = false;
  for (std::size_t index = 0U; index < marker.size(); ++index) {
    const unsigned char value =
        static_cast<unsigned char>(marker[index]);
    const bool hyphen =
        index == 8U || index == 13U || index == 18U || index == 23U;
    if (hyphen) {
      if (value != static_cast<unsigned char>('-')) {
        return false;
      }
      continue;
    }
    const bool hexadecimal =
        (value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9')) ||
        (value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('f')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('F'));
    if (!hexadecimal) {
      return false;
    }
    has_nonzero_hex =
        has_nonzero_hex || value != static_cast<unsigned char>('0');
  }
  return has_nonzero_hex;
}

clonecore::Result<bool> query_running_in_winpe() {
  HKEY key = nullptr;
  const LSTATUS status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\MiniNT",
      0U,
      KEY_QUERY_VALUE,
      &key);
  if (key != nullptr) {
    RegCloseKey(key);
  }
  if (status == ERROR_SUCCESS) {
    return clonecore::Result<bool>::success(true);
  }
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return clonecore::Result<bool>::success(false);
  }
  return failure<bool>(
      clonecore::ErrorCode::query_failed,
      static_cast<DWORD>(status),
      L"Windowsデータ救出実行環境",
      L"通常WindowsとWinPEを安全に識別できません");
}

clonecore::Result<std::vector<WindowsDataRescueMountedVolume>>
enumerate_rescue_mounted_volumes() {
  auto observed = enumerate_windows_drive_letter_volumes_read_only();
  if (!observed) {
    return clonecore::Result<
        std::vector<WindowsDataRescueMountedVolume>>::failure(
        observed.error());
  }
  std::vector<WindowsDataRescueMountedVolume> volumes;
  volumes.reserve(observed.value().size());
  for (const auto& volume : observed.value()) {
    WindowsDataRescueMountedVolume converted{
        .root_path = std::wstring{
            static_cast<wchar_t>(std::towupper(volume.drive_letter)),
            L':',
            L'\\'},
    };
    converted.disk_numbers.reserve(volume.extents.size());
    for (const auto& extent : volume.extents) {
      converted.disk_numbers.push_back(extent.disk_number);
    }
    std::sort(
        converted.disk_numbers.begin(), converted.disk_numbers.end());
    converted.disk_numbers.erase(
        std::unique(
            converted.disk_numbers.begin(), converted.disk_numbers.end()),
        converted.disk_numbers.end());
    volumes.push_back(std::move(converted));
  }
  return clonecore::Result<
      std::vector<WindowsDataRescueMountedVolume>>::success(
      std::move(volumes));
}

clonecore::Result<std::optional<std::string>> read_rescue_marker(
    const std::wstring& marker_path) {
  if (marker_path.size() < 4U || marker_path.size() >= 32U * 1024U) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"Windowsレスキュー媒体マーカーパス",
        L"マーカーパスが不正です");
  }
  const std::size_t separator = marker_path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"Windowsレスキュー媒体マーカーパス",
        L"マーカー親フォルダーを確定できません");
  }
  const std::wstring parent = marker_path.substr(0U, separator);
  const DWORD parent_attributes = GetFileAttributesW(parent.c_str());
  if (parent_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<std::optional<std::string>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Windowsレスキュー媒体マーカー親フォルダー",
            native_code));
  }
  if ((parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (parent_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"Windowsレスキュー媒体マーカー親フォルダー",
        L"通常の非reparseフォルダーではありません");
  }

  clonecore::UniqueHandle marker(CreateFileW(
      marker_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!marker) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<std::optional<std::string>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Windowsレスキュー媒体マーカーを読取り専用で開く",
            native_code));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  LARGE_INTEGER size{};
  if (!GetFileInformationByHandleEx(
          marker.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !GetFileInformationByHandleEx(
          marker.get(), FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileSizeEx(marker.get(), &size)) {
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Windowsレスキュー媒体マーカーハンドル検証",
            GetLastError()));
  }
  if ((tag.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_DEVICE)) != 0U ||
      standard.NumberOfLinks != 1U ||
      size.QuadPart != static_cast<LONGLONG>(kRescueMarkerBytes) ||
      GetFileType(marker.get()) != FILE_TYPE_DISK) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windowsレスキュー媒体マーカーハンドル検証",
        L"通常の単一リンク36バイトファイルではありません");
  }
  std::array<char, kRescueMarkerBytes> bytes{};
  DWORD read_bytes = 0U;
  if (!ReadFile(
          marker.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &read_bytes,
          nullptr) ||
      read_bytes != bytes.size()) {
    const DWORD native_code =
        read_bytes == bytes.size() ? GetLastError() : ERROR_HANDLE_EOF;
    return clonecore::Result<std::optional<std::string>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Windowsレスキュー媒体マーカー全体読取り",
            native_code));
  }
  std::string marker_value(bytes.begin(), bytes.end());
  if (!valid_rescue_marker(marker_value)) {
    return failure<std::optional<std::string>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windowsレスキュー媒体マーカー内容",
        L"有界ASCII GUID形式ではありません");
  }
  return clonecore::Result<std::optional<std::string>>::success(
      std::move(marker_value));
}

clonecore::Result<diskmodel::InventoryReport> enumerate_rescue_disks() {
  auto provider = diskmodel::make_windows_disk_inventory_provider();
  if (!provider) {
    return failure<diskmodel::InventoryReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Windowsレスキュー媒体ディスク列挙",
        L"ディスク一覧プロバイダーを作成できません");
  }
  return provider->enumerate();
}

struct TargetMarkerObservation final {
  std::wstring root_path;
  bool marker_present{};

  auto operator<=>(const TargetMarkerObservation&) const = default;
};

clonecore::Result<std::vector<TargetMarkerObservation>>
observe_target_markers(
    const clonecore::StableDiskIdentity& expected_target,
    const WindowsDataRescueProtectedTargetDependencies& dependencies) {
  auto volumes = dependencies.enumerate_mounted_volumes();
  if (!volumes) {
    return clonecore::Result<
        std::vector<TargetMarkerObservation>>::failure(volumes.error());
  }
  std::vector<TargetMarkerObservation> result;
  for (const auto& volume : volumes.value()) {
    if (!valid_drive_root(volume.root_path)) {
      return failure<std::vector<TargetMarkerObservation>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DRIVE,
          L"Windowsレスキュー媒体ドライブルート",
          L"ローカルドライブ文字形式ではありません");
    }
    std::vector<std::uint32_t> disk_numbers = volume.disk_numbers;
    std::sort(disk_numbers.begin(), disk_numbers.end());
    if (std::adjacent_find(disk_numbers.begin(), disk_numbers.end()) !=
        disk_numbers.end()) {
      return failure<std::vector<TargetMarkerObservation>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Windowsレスキュー媒体ボリューム範囲",
          L"同じ物理ディスク番号が重複しています");
    }
    if (!std::binary_search(
            disk_numbers.begin(),
            disk_numbers.end(),
            expected_target.disk_number)) {
      continue;
    }
    if (disk_numbers.size() != 1U) {
      return failure<std::vector<TargetMarkerObservation>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windowsレスキュー媒体ボリューム範囲",
          L"コピー先を含むspannedボリュームを安全に照合できません");
    }
    const std::wstring root = canonical_drive_root(volume.root_path);
    auto marker = dependencies.read_rescue_marker(
        root + std::wstring(kRescueMarkerRelativePath));
    if (!marker) {
      return clonecore::Result<
          std::vector<TargetMarkerObservation>>::failure(marker.error());
    }
    if (marker.value().has_value() &&
        !valid_rescue_marker(marker.value().value())) {
      return failure<std::vector<TargetMarkerObservation>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Windowsレスキュー媒体マーカー内容",
          L"有界ASCII GUID形式ではありません");
    }
    result.push_back({
        .root_path = root,
        .marker_present = marker.value().has_value(),
    });
  }
  std::sort(
      result.begin(),
      result.end(),
      [](const auto& left, const auto& right) {
        return left.root_path < right.root_path;
      });
  if (std::adjacent_find(
          result.begin(),
          result.end(),
          [](const auto& left, const auto& right) {
            return left.root_path == right.root_path;
          }) != result.end()) {
    return failure<std::vector<TargetMarkerObservation>>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DUP_NAME,
        L"Windowsレスキュー媒体ドライブルート",
        L"同じコピー先ドライブルートが複数回列挙されました");
  }
  return clonecore::Result<std::vector<TargetMarkerObservation>>::success(
      std::move(result));
}

bool all_zero(const imageformat::Sha256Digest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_bool(std::vector<std::byte>& bytes, const bool value) {
  append_u8(bytes, value ? std::uint8_t{1} : std::uint8_t{0});
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
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

clonecore::Status validate_rescue_observation(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    const bool target_is_protected_rescue_media) {
  if (!source.offline.has_value() || !source.read_only.has_value() ||
      !source.removable.has_value() || !target.offline.has_value() ||
      !target.read_only.has_value() || !target.removable.has_value()) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"Windowsデータ救出ディスク属性",
        L"コピー元・コピー先のoffline、read-only、removable属性を確定できません"));
  }
  if (source.is_system_disk) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"Windowsデータ救出コピー元",
        L"稼働中WindowsのシステムディスクはWindows版救出モードで読み取りません。PEを使用してください"));
  }
  if (!source.read_only.value() && !source.offline.value()) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"Windowsデータ救出コピー元保護状態",
        L"Windows版はコピー元属性を変更しません。既にread-onlyまたはofflineのデータディスクだけを救出でき、それ以外はPEを使用してください"));
  }
  if (source.size_bytes == 0U || target.size_bytes < source.size_bytes ||
      source.logical_sector_size != 512U ||
      target.logical_sector_size != source.logical_sector_size) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::unsupported_layout,
        target.size_bytes < source.size_bytes ? ERROR_DISK_FULL
                                               : ERROR_NOT_SUPPORTED,
        L"Windowsデータ救出RAW寸法",
        L"同じ512バイト論理セクターで、コピー先がコピー元と同容量以上の場合だけ救出できます"));
  }
  if (source.removable.value() || source.bus_type.empty() ||
      target.bus_type.empty()) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windowsデータ救出コピー元分類",
        L"接続方式を確定できる非removable固定データディスクだけをコピー元にできます"));
  }

  const auto source_class =
      imageformat::classify_tsumugi_physical_restore_target(source);
  if (source_class.usb_memory || source_class.dynamic_disk ||
      source_class.storage_spaces || source_class.software_raid ||
      source_class.unresolved_hardware_raid ||
      source_class.unsupported_virtual) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windowsデータ救出コピー元構成",
        L"USBメモリ、Dynamic Disk、Storage Spaces、RAID、仮想ディスクはコピー元にできません"));
  }
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(target);
  return imageformat::validate_tsumugi_physical_restore_target(
      target, target_class, target_is_protected_rescue_media);
}

clonecore::Result<operationcore::OperationId> make_operation_id() {
  GUID guid{};
  const HRESULT created = CoCreateGuid(&guid);
  if (FAILED(created)) {
    return failure<operationcore::OperationId>(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(created),
        L"Windowsデータ救出操作ID",
        L"単回操作IDを生成できません");
  }
  operationcore::OperationId id{};
  static_assert(sizeof(guid) == id.size());
  std::memcpy(id.data(), &guid, id.size());
  return clonecore::Result<operationcore::OperationId>::success(id);
}

clonecore::Result<operationcore::Sha256Digest> immutable_payload_hash(
    const WindowsDataRescueClonePlan& reviewed) {
  std::vector<std::byte> bytes;
  bytes.reserve(128U);
  append_domain(bytes, "YTEC-WINDOWS-DATA-RESCUE-CLONE-PLAN-V1");
  append_array(bytes, reviewed.expected_source_layout_hash);
  append_array(bytes, reviewed.expected_target_layout_hash);
  append_u64(bytes, reviewed.expected_source.size_bytes);
  append_u64(bytes, reviewed.expected_target.size_bytes);
  append_u64(bytes, reviewed.large_block_bytes);
  append_u8(
      bytes,
      static_cast<std::uint8_t>(reviewed.source_partition_style));
  append_bool(bytes, reviewed.expected_source.is_system_disk);
  append_bool(bytes, reviewed.source_was_read_only);
  append_bool(bytes, reviewed.source_was_offline);
  append_bool(bytes, reviewed.protected_rescue_media_checked);
  append_u8(bytes, std::uint8_t{0});  // shrink
  append_u8(bytes, std::uint8_t{0});  // partition-style conversion
  append_u8(bytes, std::uint8_t{0});  // boot finalization
  return imageformat::sha256(bytes);
}

clonecore::Result<operationcore::Sha256Digest> evidence_hash(
    const operationcore::OperationPlan& plan,
    const WindowsDataRescueCloneExecutionReport& report) {
  std::vector<std::byte> bytes;
  bytes.reserve(256U + report.raw.missing_ranges.size() * 80U);
  append_domain(bytes, "YTEC-WINDOWS-DATA-RESCUE-CLONE-EVIDENCE-V1");
  append_array(bytes, plan.immutable_payload_hash);
  append_u64(bytes, report.raw.source_extent_bytes);
  append_u64(bytes, report.raw.copied_source_bytes);
  append_u64(bytes, report.raw.recovered_bytes);
  append_u64(bytes, report.raw.zero_filled_bytes);
  append_u64(bytes, report.raw.written_and_read_back_verified_bytes);
  append_u64(bytes, report.untouched_target_tail_bytes);
  append_u64(bytes, report.raw.forward_failed_block_count);
  append_u64(bytes, report.raw.reverse_recovered_block_count);
  append_u64(bytes, report.raw.reverse_failed_block_count);
  append_u64(bytes, report.raw.sector_recovered_count);
  append_u64(bytes, report.raw.exhausted_sector_count);
  append_bool(bytes, report.raw.layout_preserved_without_conversion);
  append_bool(bytes, report.raw.byte_exact_copy);
  append_bool(bytes, report.raw.target_flushed);
  append_bool(bytes, report.raw.all_writes_read_back_verified);
  append_bool(bytes, report.raw.partial_data_loss);
  append_bool(bytes, report.source_opened_read_only);
  append_bool(bytes, report.source_was_read_only_or_offline);
  append_bool(bytes, report.source_attributes_unchanged);
  append_bool(bytes, report.target_left_offline);
  append_bool(
      bytes,
      report.protected_rescue_media_excluded_by_stable_identity);
  append_bool(bytes, report.must_display_as_partial_loss);
  append_bool(bytes, report.shrinking_performed);
  append_bool(bytes, report.partition_style_conversion_performed);
  append_bool(bytes, report.boot_finalization_performed);
  append_u32(
      bytes,
      static_cast<std::uint32_t>(report.raw.missing_ranges.size()));
  for (const auto& missing : report.raw.missing_ranges) {
    append_u64(bytes, missing.bytes.offset);
    append_u64(bytes, missing.bytes.length);
    append_u64(bytes, missing.first_lba);
    append_u64(bytes, missing.sector_count);
    append_u8(bytes, missing.forward_attempts);
    append_u8(bytes, missing.reverse_attempts);
    append_u8(bytes, missing.sector_attempts);
    append_u32(bytes, missing.forward_native_error);
    append_u32(bytes, missing.reverse_native_error);
    append_u32(bytes, missing.sector_native_error);
    append_bool(bytes, missing.zero_fill_read_back_verified);
  }
  return imageformat::sha256(bytes);
}

clonecore::Status validate_execution_report(
    const operationcore::OperationPlan& plan,
    const WindowsDataRescueCloneExecutionReport& report) {
  const auto& raw = report.raw;
  if (plan.environment != operationcore::OperationEnvironment::windows ||
      !plan.source.has_value() || plan.source->is_system_disk ||
      !plan.target.has_value() ||
      plan.target->size_bytes < plan.expected_work_bytes ||
      raw.source_extent_bytes != plan.expected_work_bytes ||
      raw.written_and_read_back_verified_bytes != plan.expected_work_bytes ||
      raw.copied_source_bytes > plan.expected_work_bytes ||
      raw.zero_filled_bytes >
          plan.expected_work_bytes - raw.copied_source_bytes ||
      raw.copied_source_bytes + raw.zero_filled_bytes !=
          plan.expected_work_bytes ||
      report.untouched_target_tail_bytes !=
          plan.target->size_bytes - plan.expected_work_bytes ||
      !raw.layout_preserved_without_conversion || !raw.target_flushed ||
      !raw.all_writes_read_back_verified ||
      (raw.partial_data_loss != !raw.missing_ranges.empty()) ||
      (raw.byte_exact_copy == raw.partial_data_loss) ||
      !report.source_opened_read_only ||
      !report.source_was_read_only_or_offline ||
      !report.source_attributes_unchanged || !report.target_left_offline ||
      !report.protected_rescue_media_excluded_by_stable_identity ||
      !report.must_display_as_partial_loss || report.shrinking_performed ||
      report.partition_style_conversion_performed ||
      report.boot_finalization_performed) {
    return clonecore::Status::failure(rescue_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Windowsデータ救出Operation最終検証",
        L"全書込み読戻し、欠損map、未処理末尾、source read-only handle/属性不変、target offline、または救出専用結果分類の証跡が不足しています"));
  }
  return clonecore::success_status();
}

clonecore::Error append_offline_failure(
    clonecore::Error primary,
    const clonecore::Status& protected_offline) {
  if (!protected_offline) {
    primary.message +=
        L"。コピー先offline状態の再確認にも失敗しました: " +
        protected_offline.error().operation;
  }
  return primary;
}

clonecore::DiskOperationProgress translate_progress(
    const clonecore::RescueCopyProgress& progress) {
  clonecore::DiskOperationStage stage =
      clonecore::DiskOperationStage::copying_data;
  if (progress.phase == clonecore::RescueCopyPhase::validating) {
    stage = clonecore::DiskOperationStage::verifying_source;
  } else if (progress.phase == clonecore::RescueCopyPhase::flushing) {
    stage = clonecore::DiskOperationStage::flushing_data;
  } else if (progress.phase == clonecore::RescueCopyPhase::completed) {
    stage = clonecore::DiskOperationStage::completed;
  }
  return clonecore::DiskOperationProgress{
      .stage = stage,
      .partition_index = std::nullopt,
      .total_read_bytes = progress.source_extent_bytes,
      .total_write_bytes = progress.source_extent_bytes,
      .total_verify_bytes = progress.source_extent_bytes,
      .read_bytes = progress.settled_target_bytes,
      .written_bytes = progress.settled_target_bytes,
      .verified_bytes = progress.settled_target_bytes,
      .cancellation_allowed = progress.cancellation_allowed,
      .pause_allowed = progress.pause_allowed,
  };
}

clonecore::Result<WindowsDataRescueCloneExecutionReport>
execute_rescue_engine(
    const WindowsDataRescueClonePlan& reviewed,
    const clonecore::TargetConfirmation& confirmation,
    const WindowsDataRescueCloneDependencies& dependencies,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (!dependencies.reidentify_selection ||
      !dependencies.is_protected_rescue_media ||
      !dependencies.open_read_only_source ||
      !dependencies.set_target_offline ||
      !dependencies.open_offline_target) {
    return failure<WindowsDataRescueCloneExecutionReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windowsデータ救出依存",
        L"再識別、保護媒体除外、read-only source open、target offline、またはI/O依存が不足しています");
  }

  auto observed = dependencies.reidentify_selection(
      reviewed.expected_source, reviewed.expected_target);
  if (!observed) {
    return clonecore::Result<WindowsDataRescueCloneExecutionReport>::failure(
        observed.error());
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!source_layout || !target_layout) {
    return clonecore::Result<WindowsDataRescueCloneExecutionReport>::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  if (source_layout.value() != reviewed.expected_source_layout_hash ||
      target_layout.value() != reviewed.expected_target_layout_hash) {
    return failure<WindowsDataRescueCloneExecutionReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windowsデータ救出実行直前レイアウト",
        L"最終確認後にコピー元またはコピー先のレイアウトが変化しました");
  }
  auto protected_target = dependencies.is_protected_rescue_media(
      observed.value().target_identity);
  if (!protected_target) {
    return clonecore::Result<WindowsDataRescueCloneExecutionReport>::failure(
        protected_target.error());
  }
  const auto ready = validate_rescue_observation(
      observed.value().source,
      observed.value().target,
      protected_target.value());
  if (!ready) {
    return clonecore::Result<WindowsDataRescueCloneExecutionReport>::failure(
        ready.error());
  }
  if (observed.value().source.read_only.value() !=
          reviewed.source_was_read_only ||
      observed.value().source.offline.value() != reviewed.source_was_offline) {
    return failure<WindowsDataRescueCloneExecutionReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windowsデータ救出コピー元属性再識別",
        L"最終確認後にコピー元のread-onlyまたはoffline属性が変化しました");
  }

  // Windows rescue never changes a source attribute. The running-system and
  // already-protected-state checks above happen before opening the RAW source.
  auto source =
      dependencies.open_read_only_source(reviewed.expected_source);
  if (!source) {
    return clonecore::Result<WindowsDataRescueCloneExecutionReport>::failure(
        source.error());
  }
  if (source.value().observed.identity.is_system_disk ||
      source.value().observed.observed.is_system_disk ||
      !source.value().observed.observed.read_only.has_value() ||
      !source.value().observed.observed.offline.has_value() ||
      (source.value().observed.observed.read_only.value() !=
       reviewed.source_was_read_only) ||
      (source.value().observed.observed.offline.value() !=
       reviewed.source_was_offline) ||
      (!reviewed.source_was_read_only && !reviewed.source_was_offline)) {
    return failure<WindowsDataRescueCloneExecutionReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_WRITE_PROTECT,
        L"Windowsデータ救出source保護状態再確認",
        L"開いたコピー元を非systemかつ属性不変のread-only/offline sourceとして再確認できません");
  }

  auto offline = dependencies.set_target_offline(
      reviewed.expected_source,
      reviewed.expected_target,
      confirmation,
      true);
  if (!offline) {
    const auto protected_offline = dependencies.set_target_offline(
        reviewed.expected_source,
        reviewed.expected_target,
        confirmation,
        true);
    return clonecore::Result<WindowsDataRescueCloneExecutionReport>::failure(
        append_offline_failure(offline.error(), protected_offline));
  }

  std::optional<diskmodel::PhysicalTargetHandle> target;
  auto fail_after_target_transition = [&](clonecore::Error error) {
    if (target.has_value()) {
      target->target.reset();
    }
    source.value().reader.reset();
    const auto protected_offline = dependencies.set_target_offline(
        reviewed.expected_source,
        reviewed.expected_target,
        confirmation,
        true);
    return clonecore::Result<
        WindowsDataRescueCloneExecutionReport>::failure(
        append_offline_failure(std::move(error), protected_offline));
  };

  auto opened_target = dependencies.open_offline_target(
      reviewed.expected_target, confirmation);
  if (!opened_target) {
    return fail_after_target_transition(opened_target.error());
  }
  target.emplace(opened_target.take_value());
  const auto identities = clonecore::validate_clone_identities(
      reviewed.expected_source,
      source.value().observed.identity,
      reviewed.expected_target,
      target->observed.target_identity,
      confirmation);
  if (!identities) {
    return fail_after_target_transition(identities.error());
  }
  if (!target->observed.target.offline.value_or(false) ||
      target->observed.target.is_system_disk) {
    return fail_after_target_transition(rescue_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"Windowsデータ救出target offline再確認",
        L"開いたコピー先を非system/offlineとして再確認できません"));
  }

  clonecore::RescueCopyCallbacks rescue_callbacks{
      .cancellation_requested = callbacks.cancellation_requested,
      .safe_boundary = callbacks.safe_boundary,
  };
  if (callbacks.progress) {
    rescue_callbacks.progress =
        [&callbacks](const clonecore::RescueCopyProgress& progress) {
          clonecore::report_disk_operation_progress(
              callbacks, translate_progress(progress));
        };
  }
  auto rescued = clonecore::execute_rescue_raw_copy(
      clonecore::RescueRawCopyRequest{
          .environment = clonecore::RescueExecutionEnvironment::windows,
          .source_kind = clonecore::RescueSourceKind::data_disk,
          .rescue_mode_explicitly_confirmed = true,
          .large_block_bytes = reviewed.large_block_bytes,
          .callbacks = std::move(rescue_callbacks),
      },
      *source.value().reader,
      *target->target);
  if (!rescued) {
    return fail_after_target_transition(rescued.error());
  }

  target->target.reset();
  target.reset();
  source.value().reader.reset();
  const auto final_offline = dependencies.set_target_offline(
      reviewed.expected_source,
      reviewed.expected_target,
      confirmation,
      true);
  if (!final_offline) {
    return fail_after_target_transition(final_offline.error());
  }
  auto final_observed = dependencies.reidentify_selection(
      reviewed.expected_source, reviewed.expected_target);
  if (!final_observed) {
    return fail_after_target_transition(final_observed.error());
  }
  auto final_source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          final_observed.value().source);
  if (!final_source_layout ||
      final_source_layout.value() != reviewed.expected_source_layout_hash ||
      final_observed.value().source.is_system_disk ||
      final_observed.value().source_identity.is_system_disk ||
      !final_observed.value().source.read_only.has_value() ||
      !final_observed.value().source.offline.has_value() ||
      final_observed.value().source.read_only.value() !=
          reviewed.source_was_read_only ||
      final_observed.value().source.offline.value() !=
          reviewed.source_was_offline ||
      !final_observed.value().target.offline.value_or(false)) {
    return fail_after_target_transition(
        !final_source_layout
            ? final_source_layout.error()
            : rescue_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windowsデータ救出最終ディスク状態",
                  L"source非system/layout/属性不変またはtarget offlineを最終確認できません"));
  }

  WindowsDataRescueCloneExecutionReport report{
      .raw = rescued.take_value(),
      .untouched_target_tail_bytes =
          reviewed.expected_target.size_bytes -
          reviewed.expected_source.size_bytes,
      .source_opened_read_only = true,
      .source_was_read_only_or_offline =
          reviewed.source_was_read_only || reviewed.source_was_offline,
      .source_attributes_unchanged = true,
      .target_left_offline = true,
      .protected_rescue_media_excluded_by_stable_identity = true,
      .must_display_as_partial_loss = true,
      .shrinking_performed = false,
      .partition_style_conversion_performed = false,
      .boot_finalization_performed = false,
  };
  const auto valid_report = validate_execution_report(
      operationcore::OperationPlan{
          .operation_id = reviewed.operation_id,
          .kind = operationcore::OperationKind::rescue_clone,
          .environment = operationcore::OperationEnvironment::windows,
          .source = reviewed.expected_source,
          .target = reviewed.expected_target,
          .expected_work_bytes = reviewed.expected_source.size_bytes,
          .immutable_payload_hash = reviewed.expected_source_layout_hash,
      },
      report);
  if (!valid_report) {
    return fail_after_target_transition(valid_report.error());
  }
  return clonecore::Result<WindowsDataRescueCloneExecutionReport>::success(
      std::move(report));
}

clonecore::Result<operationcore::ReidentifiedOperation>
reidentify_for_operation(
    const WindowsDataRescueClonePlan& reviewed,
    const WindowsDataRescueCloneDependencies& dependencies) {
  if (!dependencies.reidentify_selection ||
      !dependencies.is_protected_rescue_media) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windowsデータ救出Operation再識別依存",
        L"再識別または保護媒体除外依存がありません");
  }
  auto observed = dependencies.reidentify_selection(
      reviewed.expected_source, reviewed.expected_target);
  if (!observed) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        observed.error());
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!source_layout || !target_layout) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  auto protected_target = dependencies.is_protected_rescue_media(
      observed.value().target_identity);
  if (!protected_target) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        protected_target.error());
  }
  const auto ready = validate_rescue_observation(
      observed.value().source,
      observed.value().target,
      protected_target.value());
  if (!ready) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        ready.error());
  }
  if (source_layout.value() != reviewed.expected_source_layout_hash ||
      target_layout.value() != reviewed.expected_target_layout_hash) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windowsデータ救出Operationレイアウト再識別",
        L"画面で確認したコピー元またはコピー先のレイアウトが変化しました");
  }
  return clonecore::Result<operationcore::ReidentifiedOperation>::success({
      .source = observed.value().source_identity,
      .target = observed.value().target_identity,
  });
}

}  // namespace

clonecore::Result<bool> resolve_windows_data_rescue_protected_target(
    const clonecore::StableDiskIdentity& expected_target,
    const WindowsDataRescueProtectedTargetDependencies& dependencies) {
  if (!dependencies.running_in_winpe ||
      !dependencies.enumerate_mounted_volumes ||
      !dependencies.read_rescue_marker || !dependencies.enumerate_disks) {
    return failure<bool>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"Windowsレスキュー媒体除外依存",
        L"実行環境、volume、marker、またはdiskの読取り専用照合依存が不足しています");
  }
  const auto expected_valid = clonecore::validate_stable_identity(
      expected_target, expected_target, L"Windowsデータ救出コピー先");
  if (!expected_valid) {
    return clonecore::Result<bool>::failure(expected_valid.error());
  }
  auto in_winpe = dependencies.running_in_winpe();
  if (!in_winpe) {
    return clonecore::Result<bool>::failure(in_winpe.error());
  }
  if (in_winpe.value()) {
    return failure<bool>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windowsデータ救出実行環境",
        L"Windows版救出経路をWinPEから実行できません。PE版の直接救出モードを使用してください");
  }

  auto before = observe_target_markers(expected_target, dependencies);
  if (!before) {
    return clonecore::Result<bool>::failure(before.error());
  }
  auto inventory = dependencies.enumerate_disks();
  if (!inventory) {
    return clonecore::Result<bool>::failure(inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return failure<bool>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"Windowsレスキュー媒体ディスク再識別",
        L"未解決のディスク列挙診断があるため保護媒体を判定できません");
  }
  const auto same_number = [&](const diskmodel::DiskInfo& disk) {
    return disk.disk_number == expected_target.disk_number;
  };
  const std::size_t count = static_cast<std::size_t>(std::count_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      same_number));
  if (count != 1U) {
    return failure<bool>(
        clonecore::ErrorCode::identity_mismatch,
        count == 0U ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"Windowsレスキュー媒体ディスク再識別",
        L"コピー先ディスク番号を一意に再識別できません");
  }
  const auto observed = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      same_number);
  auto observed_identity = diskmodel::make_stable_disk_identity(
      *observed, observed->is_system_disk);
  if (!observed_identity) {
    return clonecore::Result<bool>::failure(observed_identity.error());
  }
  const auto identity_valid = clonecore::validate_stable_identity(
      expected_target,
      observed_identity.value(),
      L"Windowsレスキュー媒体コピー先");
  if (!identity_valid) {
    return clonecore::Result<bool>::failure(identity_valid.error());
  }
  auto after = observe_target_markers(expected_target, dependencies);
  if (!after) {
    return clonecore::Result<bool>::failure(after.error());
  }
  if (before.value() != after.value()) {
    return failure<bool>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windowsレスキュー媒体volume再照合",
        L"保護媒体の読取り専用確認中にコピー先volumeまたはmarkerが変化しました");
  }
  const bool protected_media = std::any_of(
      after.value().begin(),
      after.value().end(),
      [](const TargetMarkerObservation& item) {
        return item.marker_present;
      });
  return clonecore::Result<bool>::success(protected_media);
}

WindowsDataRescueProtectedTargetDependencies
make_windows_data_rescue_protected_target_dependencies() {
  return WindowsDataRescueProtectedTargetDependencies{
      .running_in_winpe = query_running_in_winpe,
      .enumerate_mounted_volumes = enumerate_rescue_mounted_volumes,
      .read_rescue_marker = read_rescue_marker,
      .enumerate_disks = enumerate_rescue_disks,
  };
}

clonecore::Result<bool>
query_windows_data_rescue_protected_target_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target) {
  return resolve_windows_data_rescue_protected_target(
      expected_target,
      make_windows_data_rescue_protected_target_dependencies());
}

clonecore::Result<WindowsDataRescueClonePlan>
prepare_windows_data_rescue_clone(
    const std::uint32_t source_disk_number,
    const std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider,
    const WindowsDataRescueProtectedTargetQuery& protected_target_query) {
  if (!protected_target_query) {
    return failure<WindowsDataRescueClonePlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windowsデータ救出保護媒体除外",
        L"保護対象レスキュー媒体の確認機能がありません");
  }
  auto inventory = provider.enumerate();
  if (!inventory) {
    return clonecore::Result<WindowsDataRescueClonePlan>::failure(
        inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return failure<WindowsDataRescueClonePlan>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"Windowsデータ救出全ディスク列挙",
        L"未解決の列挙診断があるため対象を選択できません");
  }
  const auto source = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [source_disk_number](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == source_disk_number;
      });
  const auto target = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [target_disk_number](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == target_disk_number;
      });
  if (source == inventory.value().disks.end() ||
      target == inventory.value().disks.end() || source == target) {
    return failure<WindowsDataRescueClonePlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"Windowsデータ救出対象選択",
        L"別々のコピー元とコピー先ディスクを選択してください");
  }
  if (source->is_system_disk) {
    return failure<WindowsDataRescueClonePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"Windowsデータ救出コピー元レビュー",
        L"稼働中WindowsのシステムディスクはI/O前に拒否しました。PEを使用してください");
  }

  auto source_identity = diskmodel::make_stable_disk_identity(*source, false);
  auto target_identity = diskmodel::make_stable_disk_identity(
      *target, target->is_system_disk);
  if (!source_identity || !target_identity) {
    return clonecore::Result<WindowsDataRescueClonePlan>::failure(
        !source_identity ? source_identity.error() : target_identity.error());
  }
  const auto identities = clonecore::validate_clone_selection(
      source_identity.value(),
      source_identity.value(),
      target_identity.value(),
      target_identity.value());
  if (!identities) {
    return clonecore::Result<WindowsDataRescueClonePlan>::failure(
        identities.error());
  }
  auto protected_target = protected_target_query(target_identity.value());
  if (!protected_target) {
    return clonecore::Result<WindowsDataRescueClonePlan>::failure(
        protected_target.error());
  }
  const auto ready = validate_rescue_observation(
      *source, *target, protected_target.value());
  if (!ready) {
    return clonecore::Result<WindowsDataRescueClonePlan>::failure(
        ready.error());
  }

  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(*source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(*target);
  auto operation_id = make_operation_id();
  if (!source_layout || !target_layout || !operation_id) {
    if (!source_layout) {
      return clonecore::Result<WindowsDataRescueClonePlan>::failure(
          source_layout.error());
    }
    if (!target_layout) {
      return clonecore::Result<WindowsDataRescueClonePlan>::failure(
          target_layout.error());
    }
    return clonecore::Result<WindowsDataRescueClonePlan>::failure(
        operation_id.error());
  }
  return clonecore::Result<WindowsDataRescueClonePlan>::success({
      .operation_id = operation_id.take_value(),
      .expected_source = source_identity.take_value(),
      .expected_target = target_identity.take_value(),
      .expected_source_layout_hash = source_layout.take_value(),
      .expected_target_layout_hash = target_layout.take_value(),
      .source_partition_style = source->partition_style,
      .source_bus_type = source->bus_type,
      .target_bus_type = target->bus_type,
      .source_partition_count = source->partitions.size(),
      .target_partition_count = target->partitions.size(),
      .source_health = source->health,
      .target_health = target->health,
      .large_block_bytes = 4U * 1024U * 1024U,
      .source_was_read_only = source->read_only.value(),
      .source_was_offline = source->offline.value(),
      .protected_rescue_media_checked = true,
  });
}

clonecore::Result<WindowsDataRescueCloneOperationReport>
execute_windows_data_rescue_clone(
    const WindowsDataRescueClonePlan& reviewed_plan,
    const bool target_erasure_acknowledged,
    const std::wstring_view typed_confirmation,
    const WindowsDataRescueCloneDependencies& dependencies,
    clonecore::DiskOperationCallbacks callbacks) {
  if (!target_erasure_acknowledged) {
    return failure<WindowsDataRescueCloneOperationReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"Windowsデータ救出消去確認",
        L"コピー先ディスク全体の消去内容を確認してください");
  }
  if (reviewed_plan.expected_source.is_system_disk ||
      !reviewed_plan.protected_rescue_media_checked ||
      (!reviewed_plan.source_was_read_only &&
       !reviewed_plan.source_was_offline) ||
      reviewed_plan.expected_source.logical_sector_size == 0U ||
      reviewed_plan.large_block_bytes == 0U ||
      reviewed_plan.large_block_bytes > 16U * 1024U * 1024U ||
      reviewed_plan.large_block_bytes %
              reviewed_plan.expected_source.logical_sector_size !=
          0U ||
      all_zero(reviewed_plan.expected_source_layout_hash) ||
      all_zero(reviewed_plan.expected_target_layout_hash)) {
    return failure<WindowsDataRescueCloneOperationReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windowsデータ救出レビュー",
        L"非system/既保護source、保護媒体除外、レイアウトHash、または救出ブロック設定が不正です");
  }
  auto payload_hash = immutable_payload_hash(reviewed_plan);
  if (!payload_hash) {
    return clonecore::Result<WindowsDataRescueCloneOperationReport>::failure(
        payload_hash.error());
  }
  operationcore::OperationPlan plan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = reviewed_plan.operation_id,
      .kind = operationcore::OperationKind::rescue_clone,
      .environment = operationcore::OperationEnvironment::windows,
      .source = reviewed_plan.expected_source,
      .target = reviewed_plan.expected_target,
      .expected_work_bytes = reviewed_plan.expected_source.size_bytes,
      .immutable_payload_hash = payload_hash.take_value(),
  };
  const auto plan_valid = operationcore::validate_operation_plan(plan);
  if (!plan_valid) {
    return clonecore::Result<WindowsDataRescueCloneOperationReport>::failure(
        plan_valid.error());
  }

  const clonecore::TargetConfirmation target_confirmation{
      .first_step_acknowledged = true,
      .typed_token = L"OK",
  };
  std::optional<WindowsDataRescueCloneExecutionReport> rescue;
  operationcore::OperationCallbacks operation_callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan&) {
            return reidentify_for_operation(reviewed_plan, dependencies);
          },
      .execute =
          [&](const operationcore::OperationPlan& current,
              const clonecore::DiskOperationCallbacks& operation_progress) {
            auto executed = execute_rescue_engine(
                reviewed_plan,
                target_confirmation,
                dependencies,
                operation_progress);
            if (!executed) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  executed.error());
            }
            rescue = executed.take_value();
            const auto valid_report =
                validate_execution_report(current, rescue.value());
            if (!valid_report) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  valid_report.error());
            }
            auto evidence = evidence_hash(current, rescue.value());
            if (!evidence) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  evidence.error());
            }
            return clonecore::Result<
                operationcore::ExecutionEvidence>::success({
                .processed_work_bytes = current.expected_work_bytes,
                .output_hash = evidence.take_value(),
            });
          },
      .verify =
          [&](const operationcore::OperationPlan& current,
              const operationcore::ExecutionEvidence& execution,
              const clonecore::DiskOperationCallbacks&) {
            if (!rescue.has_value()) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windowsデータ救出Operation証跡",
                  L"救出実行結果がありません");
            }
            const auto valid_report =
                validate_execution_report(current, rescue.value());
            if (!valid_report) {
              return clonecore::Result<
                  operationcore::VerificationEvidence>::failure(
                  valid_report.error());
            }
            auto evidence = evidence_hash(current, rescue.value());
            if (!evidence) {
              return clonecore::Result<
                  operationcore::VerificationEvidence>::failure(
                  evidence.error());
            }
            if (execution.output_hash != evidence.value()) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windowsデータ救出Operation Hash照合",
                  L"実行時と最終検証時の救出証跡が一致しません");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = current.expected_work_bytes,
                .output_hash = evidence.take_value(),
            });
          },
      .disk_operation = std::move(callbacks),
  };
  auto lifecycle = operationcore::run_operation(
      plan, typed_confirmation, operation_callbacks);
  return clonecore::Result<WindowsDataRescueCloneOperationReport>::success({
      .plan = std::move(plan),
      .lifecycle = std::move(lifecycle),
      .rescue = std::move(rescue),
  });
}

WindowsDataRescueCloneDependencies
make_windows_data_rescue_clone_dependencies(
    WindowsDataRescueProtectedTargetQuery protected_target_query) {
  return WindowsDataRescueCloneDependencies{
      .reidentify_selection =
          [](const clonecore::StableDiskIdentity& source,
             const clonecore::StableDiskIdentity& target) {
            auto inventory = diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone_selection(
                source, target, *inventory, true);
          },
      .is_protected_rescue_media = std::move(protected_target_query),
      .open_read_only_source =
          diskmodel::open_verified_read_only_physical_disk_with_windows_apis,
      .set_target_offline =
          diskmodel::set_verified_target_offline_with_windows_apis,
      .open_offline_target =
          diskmodel::open_verified_physical_target_with_windows_apis,
  };
}

std::wstring format_windows_data_rescue_clone_result(
    const WindowsDataRescueCloneExecutionReport& report) {
  const auto& raw = report.raw;
  std::wostringstream stream;
  stream
      << kWindowsDataRescueCloneResultClassification
      << L"\r\nこれは通常クローン成功・起動成功の表示ではありません。\r\n\r\n"
         L"対象範囲: "
      << raw.source_extent_bytes << L" bytes"
      << L"\r\n読取回復済み: " << raw.copied_source_bytes << L" bytes"
      << L"\r\n再試行で回復: " << raw.recovered_bytes << L" bytes"
      << L"\r\nゼロ埋め: " << raw.zero_filled_bytes << L" bytes"
      << L"\r\n全書込み読戻し: "
      << (raw.all_writes_read_back_verified ? L"合格" : L"未完了")
      << L"\r\nコピー先flush: "
      << (raw.target_flushed ? L"完了" : L"未完了")
      << L"\r\nコピー先の未処理末尾: "
      << report.untouched_target_tail_bytes
      << L" bytes（コピー元容量を超える範囲は消去・検証していません）"
      << L"\r\nコピー元read-only handle: "
      << (report.source_opened_read_only ? L"確認済み" : L"未確認")
      << L"\r\nコピー元の事前保護状態: "
      << (report.source_was_read_only_or_offline ? L"確認済み" : L"未確認")
      << L"\r\nコピー元属性の非変更: "
      << (report.source_attributes_unchanged ? L"確認済み" : L"未確認")
      << L"\r\nコピー先offline保持: "
      << (report.target_left_offline ? L"確認済み" : L"未確認")
      << L"\r\n\r\n実欠損map: ";
  if (!raw.partial_data_loss) {
    stream
        << L"0件（全範囲を回復）\r\n"
           L"ただし救出処理のため、結果分類は「一部欠損の可能性あり」のままです。";
    return stream.str();
  }

  stream << raw.missing_ranges.size() << L"件 / "
         << raw.zero_filled_bytes << L" bytes\r\n";
  constexpr std::size_t kDisplayedMissingRangeLimit = 32U;
  const std::size_t displayed =
      (std::min)(raw.missing_ranges.size(), kDisplayedMissingRangeLimit);
  for (std::size_t index = 0; index < displayed; ++index) {
    const auto& missing = raw.missing_ranges[index];
    stream << L"  [" << index + 1U << L"] offset "
           << missing.bytes.offset << L" / length "
           << missing.bytes.length << L" / LBA "
           << missing.first_lba << L" / sectors "
           << missing.sector_count << L" / zero-fill読戻し "
           << (missing.zero_fill_read_back_verified ? L"合格" : L"未完了")
           << L"\r\n";
  }
  if (displayed < raw.missing_ranges.size()) {
    stream << L"  ...ほか " << raw.missing_ranges.size() - displayed
           << L"件（画面の安全な表示上限により省略）\r\n";
  }
  return stream.str();
}

}  // namespace ytec::windowsapp

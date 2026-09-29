#include "ytec/clonecore/windows_volume_bitmap.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::clonecore {
namespace {

constexpr DWORD kBitmapBufferSize = 64U * 1024U;
constexpr std::size_t kMaximumUsedRanges = 1'000'000;

Error bitmap_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool is_hex(const wchar_t value) {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

bool has_valid_volume_guid_path(const std::wstring_view path) {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (path.size() != 49 || !path.starts_with(prefix) ||
      path[47] != L'}' || path[48] != L'\\') {
    return false;
  }
  for (std::size_t index = prefix.size(); index < 47; ++index) {
    const std::size_t guid_index = index - prefix.size();
    const bool expects_hyphen =
        guid_index == 8 || guid_index == 13 || guid_index == 18 ||
        guid_index == 23;
    if ((expects_hyphen && path[index] != L'-') ||
        (!expects_hyphen && !is_hex(path[index]))) {
      return false;
    }
  }
  return true;
}

bool has_valid_snapshot_device_path(const std::wstring_view path) {
  constexpr std::wstring_view prefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (!path.starts_with(prefix) || path.size() <= prefix.size()) {
    return false;
  }
  std::wstring_view suffix = path.substr(prefix.size());
  if (suffix.ends_with(L'\\')) {
    suffix.remove_suffix(1);
  }
  return !suffix.empty() &&
         std::all_of(
             suffix.begin(),
             suffix.end(),
             [](const wchar_t value) {
               return value >= L'0' && value <= L'9';
             });
}

void append_coalesced(
    std::vector<ByteRange>& destination,
    const ByteRange& range) {
  if (!destination.empty()) {
    ByteRange& last = destination.back();
    std::uint64_t last_end{};
    if (checked_add(last.offset, last.length, last_end) &&
        last_end == range.offset) {
      last.length += range.length;
      return;
    }
  }
  destination.push_back(range);
}

Result<std::vector<ByteRange>> query_used_ranges_from_path(
    std::wstring device_path,
    const NtfsGeometry& geometry) {
  if (!device_path.empty() && device_path.back() == L'\\') {
    device_path.pop_back();
  }
  UniqueHandle volume(CreateFileW(
      device_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    const DWORD native_code = GetLastError();
    return Result<std::vector<ByteRange>>::failure(make_win32_error(
        native_code == ERROR_ACCESS_DENIED ? ErrorCode::access_denied
                                           : ErrorCode::io_failed,
        L"NTFSボリュームの読み取り専用オープン",
        native_code));
  }

  // NTFS can leave sectors at the end of a volume that do not form a complete
  // allocation unit. The volume bitmap describes complete clusters only.
  const std::uint64_t volume_cluster_count = geometry.complete_cluster_count();
  const std::uint64_t cluster_size = geometry.cluster_size();
  std::vector<ByteRange> used_ranges;
  std::uint64_t requested_lcn = 0;
  while (requested_lcn < volume_cluster_count) {
    STARTING_LCN_INPUT_BUFFER input{};
    input.StartingLcn.QuadPart = static_cast<LONGLONG>(requested_lcn);
    std::vector<std::byte> output(kBitmapBufferSize, std::byte{0});
    DWORD bytes_returned = 0;
    const BOOL success = DeviceIoControl(
        volume.get(),
        FSCTL_GET_VOLUME_BITMAP,
        &input,
        sizeof(input),
        output.data(),
        static_cast<DWORD>(output.size()),
        &bytes_returned,
        nullptr);
    const DWORD native_code = success ? ERROR_SUCCESS : GetLastError();
    if (!success && native_code != ERROR_MORE_DATA) {
      Error failure = make_win32_error(
          native_code == ERROR_ACCESS_DENIED ? ErrorCode::access_denied
                                             : ErrorCode::query_failed,
          L"FSCTL_GET_VOLUME_BITMAP",
          native_code);
      failure.message +=
          L" [requestedLcn=" + std::to_wstring(requested_lcn) +
          L", volumeClusterCount=" + std::to_wstring(volume_cluster_count) +
          L", bytesReturned=" + std::to_wstring(bytes_returned) + L"]";
      return Result<std::vector<ByteRange>>::failure(std::move(failure));
    }
    const std::size_t bitmap_offset =
        FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer);
    if (bytes_returned <= bitmap_offset) {
      return Result<std::vector<ByteRange>>::failure(bitmap_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"FSCTL_GET_VOLUME_BITMAP応答長",
          L"ボリュームビットマップ応答が短すぎます"));
    }
    VOLUME_BITMAP_BUFFER header{};
    std::memcpy(&header, output.data(), bitmap_offset);
    if (header.StartingLcn.QuadPart < 0 || header.BitmapSize.QuadPart <= 0) {
      return Result<std::vector<ByteRange>>::failure(bitmap_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"FSCTL_GET_VOLUME_BITMAP応答値",
          L"ボリュームビットマップのLCNまたはビット数が不正です"));
    }
    const std::span<const std::byte> bitmap(
        output.data() + bitmap_offset, bytes_returned - bitmap_offset);
    const auto decoded = decode_volume_bitmap_chunk(
        static_cast<std::uint64_t>(header.StartingLcn.QuadPart),
        static_cast<std::uint64_t>(header.BitmapSize.QuadPart),
        bitmap,
        requested_lcn,
        volume_cluster_count,
        cluster_size);
    if (!decoded) {
      return Result<std::vector<ByteRange>>::failure(decoded.error());
    }
    for (const auto& range : decoded.value().used_ranges) {
      append_coalesced(used_ranges, range);
      if (used_ranges.size() > kMaximumUsedRanges) {
        return Result<std::vector<ByteRange>>::failure(bitmap_error(
            ErrorCode::unsupported_layout,
            ERROR_NOT_ENOUGH_MEMORY,
            L"NTFS使用クラスタ断片数",
            L"使用クラスタの断片数が安全な処理上限を超えました"));
      }
    }
    if (decoded.value().next_lcn <= requested_lcn) {
      return Result<std::vector<ByteRange>>::failure(bitmap_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"FSCTL_GET_VOLUME_BITMAP反復",
          L"ボリュームビットマップの反復位置が前進しません"));
    }
    requested_lcn = decoded.value().next_lcn;
    if (success && requested_lcn < volume_cluster_count) {
      return Result<std::vector<ByteRange>>::failure(bitmap_error(
          ErrorCode::invalid_data,
          ERROR_HANDLE_EOF,
          L"FSCTL_GET_VOLUME_BITMAP完了",
          L"成功応答がボリューム末尾までのビットを含んでいません"));
    }
  }
  if (used_ranges.empty()) {
    return Result<std::vector<ByteRange>>::failure(bitmap_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"NTFS使用クラスタ結果",
        L"使用クラスタを1つも取得できませんでした"));
  }
  return Result<std::vector<ByteRange>>::success(std::move(used_ranges));
}

}  // namespace

Result<DecodedVolumeBitmapChunk> decode_volume_bitmap_chunk(
    const std::uint64_t returned_starting_lcn,
    const std::uint64_t returned_bitmap_size,
    const std::span<const std::byte> bitmap_bytes,
    const std::uint64_t requested_lcn,
    const std::uint64_t volume_cluster_count,
    const std::uint64_t cluster_size) {
  if (cluster_size == 0 || returned_bitmap_size == 0 ||
      returned_starting_lcn > requested_lcn ||
      requested_lcn >= volume_cluster_count || bitmap_bytes.empty()) {
    return Result<DecodedVolumeBitmapChunk>::failure(bitmap_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"NTFSボリュームビットマップ応答",
        L"開始LCN、ビット数、またはクラスタ寸法が不正です"));
  }
  const std::uint64_t available_bits = std::min<std::uint64_t>(
      returned_bitmap_size,
      static_cast<std::uint64_t>(bitmap_bytes.size()) * 8U);
  std::uint64_t returned_end{};
  if (!checked_add(returned_starting_lcn, available_bits, returned_end) ||
      returned_end <= requested_lcn) {
    return Result<DecodedVolumeBitmapChunk>::failure(bitmap_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"NTFSボリュームビットマップ進行",
        L"ビットマップ応答が前進していません"));
  }
  const std::uint64_t process_end =
      std::min(returned_end, volume_cluster_count);
  DecodedVolumeBitmapChunk decoded;
  decoded.next_lcn = process_end;
  for (std::uint64_t lcn = requested_lcn; lcn < process_end; ++lcn) {
    const std::uint64_t bit_index = lcn - returned_starting_lcn;
    const std::size_t byte_index = static_cast<std::size_t>(bit_index / 8U);
    const std::uint8_t bit_mask =
        static_cast<std::uint8_t>(1U << (bit_index % 8U));
    const bool used =
        (std::to_integer<std::uint8_t>(bitmap_bytes[byte_index]) & bit_mask) != 0;
    if (!used) {
      continue;
    }
    std::uint64_t offset{};
    if (!checked_multiply(lcn, cluster_size, offset)) {
      return Result<DecodedVolumeBitmapChunk>::failure(bitmap_error(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"NTFS使用クラスタ位置",
          L"クラスタ位置のバイト変換がオーバーフローしました"));
    }
    append_coalesced(
        decoded.used_ranges, ByteRange{.offset = offset, .length = cluster_size});
    if (decoded.used_ranges.size() > kMaximumUsedRanges) {
      return Result<DecodedVolumeBitmapChunk>::failure(bitmap_error(
          ErrorCode::unsupported_layout,
          ERROR_NOT_ENOUGH_MEMORY,
          L"NTFS使用クラスタ断片数",
          L"使用クラスタの断片数が安全な処理上限を超えました"));
    }
  }
  return Result<DecodedVolumeBitmapChunk>::success(std::move(decoded));
}

WindowsVolumeBitmapProvider::WindowsVolumeBitmapProvider(
    std::vector<VolumeBitmapBinding> bindings)
    : bindings_(std::move(bindings)) {}

Result<std::vector<ByteRange>> WindowsVolumeBitmapProvider::query_used_ranges(
    const std::uint32_t partition_index,
    const NtfsGeometry& geometry) {
  const auto binding = std::find_if(
      bindings_.begin(), bindings_.end(), [&](const auto& candidate) {
        return candidate.partition_entry_index == partition_index;
      });
  const auto binding_count = std::count_if(
      bindings_.begin(), bindings_.end(), [&](const auto& candidate) {
        return candidate.partition_entry_index == partition_index;
      });
  if (binding_count != 1 || binding == bindings_.end() ||
      !has_valid_volume_guid_path(binding->volume_device_path)) {
    return Result<std::vector<ByteRange>>::failure(bitmap_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"NTFSボリューム対応付け",
        L"対象パーティションに安全なVolume GUIDパスが対応付けられていません"));
  }
  return query_used_ranges_from_path(
      binding->volume_device_path,
      geometry);
}

WindowsSnapshotVolumeBitmapProvider::WindowsSnapshotVolumeBitmapProvider(
    std::vector<SnapshotVolumeBitmapBinding> bindings)
    : bindings_(std::move(bindings)) {}

Result<std::vector<ByteRange>>
WindowsSnapshotVolumeBitmapProvider::query_used_ranges(
    const std::uint32_t partition_index,
    const NtfsGeometry& geometry) {
  const auto binding = std::find_if(
      bindings_.begin(), bindings_.end(), [&](const auto& candidate) {
        return candidate.partition_entry_index == partition_index;
      });
  const auto binding_count = std::count_if(
      bindings_.begin(), bindings_.end(), [&](const auto& candidate) {
        return candidate.partition_entry_index == partition_index;
      });
  if (binding_count != 1 || binding == bindings_.end() ||
      !has_valid_snapshot_device_path(binding->snapshot_device_path)) {
    return Result<std::vector<ByteRange>>::failure(bitmap_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"VSS Snapshotボリューム対応付け",
        L"対象パーティションに検証済みSnapshotデバイスが対応付けられていません"));
  }
  return query_used_ranges_from_path(
      binding->snapshot_device_path,
      geometry);
}

}  // namespace ytec::clonecore

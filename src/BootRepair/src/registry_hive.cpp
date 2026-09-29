#include "ytec/bootrepair/registry_hive.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::size_t kBaseBlockBytes = 4096;
constexpr std::size_t kBaseChecksumOffset = 0x1FC;
constexpr std::size_t kHiveBinHeaderBytes = 32;
constexpr std::uint64_t kMaximumHiveBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kReadChunkBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kNoCell = 0xFFFFFFFFU;
constexpr std::size_t kMaximumSubkeys = 1U << 20U;
constexpr std::size_t kMaximumValues = 4096;
constexpr std::size_t kMaximumKeyNameBytes = 512;
constexpr std::size_t kMaximumValueNameBytes = 32768;
constexpr std::size_t kMaximumVersionValueBytes = 512;
constexpr std::size_t kMaximumIndexDepth = 8;
constexpr std::uint16_t kKeyCompressedName = 0x0020;
constexpr std::uint16_t kValueCompressedName = 0x0001;

clonecore::Error hive_error(
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
clonecore::Result<T> invalid_hive(std::wstring message) {
  return clonecore::Result<T>::failure(hive_error(
      clonecore::ErrorCode::invalid_data,
      ERROR_BADDB,
      L"オフラインWindows SOFTWARE hive解析",
      std::move(message)));
}

bool has_bytes(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t count) noexcept {
  return offset <= bytes.size() && count <= bytes.size() - offset;
}

clonecore::Result<std::uint16_t> read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  if (!has_bytes(bytes, offset, sizeof(std::uint16_t))) {
    return invalid_hive<std::uint16_t>(L"16ビット値が範囲外です");
  }
  return clonecore::Result<std::uint16_t>::success(
      static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(bytes[offset])) |
      static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(bytes[offset + 1]))
          << 8U);
}

clonecore::Result<std::uint32_t> read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  if (!has_bytes(bytes, offset, sizeof(std::uint32_t))) {
    return invalid_hive<std::uint32_t>(L"32ビット値が範囲外です");
  }
  std::uint32_t value{};
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return clonecore::Result<std::uint32_t>::success(value);
}

clonecore::Result<std::int32_t> read_i32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  const auto value = read_u32(bytes, offset);
  if (!value) {
    return clonecore::Result<std::int32_t>::failure(value.error());
  }
  return clonecore::Result<std::int32_t>::success(
      static_cast<std::int32_t>(value.value()));
}

bool has_signature(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const char first,
    const char second) noexcept {
  return has_bytes(bytes, offset, 2) &&
      std::to_integer<unsigned char>(bytes[offset]) ==
          static_cast<unsigned char>(first) &&
      std::to_integer<unsigned char>(bytes[offset + 1]) ==
          static_cast<unsigned char>(second);
}

clonecore::Result<std::vector<std::byte>> read_hive_file(
    const std::wstring& path) {
  clonecore::UniqueHandle file(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"オフラインWindows SOFTWARE hive読取り",
            GetLastError()));
  }

  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          file.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"オフラインWindows SOFTWARE hive属性取得",
            GetLastError()));
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return clonecore::Result<std::vector<std::byte>>::failure(hive_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"オフラインWindows SOFTWARE hive検証",
        L"ディレクトリまたはreparse pointは使用できません"));
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"オフラインWindows SOFTWARE hiveサイズ取得",
            GetLastError()));
  }
  if (size.QuadPart < static_cast<LONGLONG>(kBaseBlockBytes) ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaximumHiveBytes) {
    return invalid_hive<std::vector<std::byte>>(
        L"hiveサイズが許可範囲外です");
  }

  std::vector<std::byte> bytes;
  try {
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
  } catch (const std::bad_alloc&) {
    return clonecore::Result<std::vector<std::byte>>::failure(hive_error(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"オフラインWindows SOFTWARE hive読取り",
        L"hiveを検証するための有界メモリを確保できません"));
  }

  std::size_t total{};
  while (total < bytes.size()) {
    const std::size_t remaining = bytes.size() - total;
    const DWORD request = static_cast<DWORD>(
        (std::min)(remaining, kReadChunkBytes));
    DWORD received{};
    if (!ReadFile(
            file.get(), bytes.data() + total, request, &received, nullptr)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"オフラインWindows SOFTWARE hive読取り",
              GetLastError()));
    }
    if (received == 0 || received > request) {
      return clonecore::Result<std::vector<std::byte>>::failure(hive_error(
          clonecore::ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"オフラインWindows SOFTWARE hive読取り",
          L"hiveを最後まで読み取れません"));
    }
    total += received;
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

struct CellRecord final {
  std::uint32_t relative_offset{};
  std::size_t file_offset{};
  std::size_t size{};
};

struct KeyNode final {
  std::uint32_t subkey_count{};
  std::uint32_t subkey_list{kNoCell};
  std::uint32_t value_count{};
  std::uint32_t value_list{kNoCell};
  std::wstring name;
};

struct RegistryValue final {
  std::uint32_t type{};
  std::vector<std::byte> data;
};

class HiveParser final {
 public:
  explicit HiveParser(std::span<const std::byte> bytes) : bytes_(bytes) {}

  clonecore::Status validate() {
    if (!has_bytes(bytes_, 0, kBaseBlockBytes) ||
        !has_signature(bytes_, 0, 'r', 'e') ||
        !has_signature(bytes_, 2, 'g', 'f')) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"regfベースブロックがありません").error());
    }
    const auto primary_sequence = read_u32(bytes_, 4);
    const auto secondary_sequence = read_u32(bytes_, 8);
    const auto file_type = read_u32(bytes_, 0x1C);
    const auto file_format = read_u32(bytes_, 0x20);
    const auto root_cell = read_u32(bytes_, 0x24);
    const auto bins_bytes = read_u32(bytes_, 0x28);
    const auto stored_checksum = read_u32(bytes_, kBaseChecksumOffset);
    if (!primary_sequence || !secondary_sequence || !file_type ||
        !file_format || !root_cell || !bins_bytes || !stored_checksum) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"ベースブロックが不完全です").error());
    }
    if (primary_sequence.value() != secondary_sequence.value()) {
      return clonecore::Status::failure(invalid_hive<int>(
          L"未反映トランザクションのあるhiveは読み取りません").error());
    }
    if (file_type.value() != 0 || file_format.value() != 1) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"未対応のhive形式です").error());
    }

    std::uint32_t checksum{};
    for (std::size_t offset = 0; offset < kBaseChecksumOffset;
         offset += sizeof(std::uint32_t)) {
      const auto value = read_u32(bytes_, offset);
      if (!value) {
        return clonecore::Status::failure(value.error());
      }
      checksum ^= value.value();
    }
    if (checksum == 0xFFFFFFFFU) {
      checksum = 0xFFFFFFFEU;
    } else if (checksum == 0U) {
      checksum = 1U;
    }
    if (checksum != stored_checksum.value()) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"ベースブロックchecksumが一致しません").error());
    }

    if (bins_bytes.value() == 0 ||
        bins_bytes.value() % kBaseBlockBytes != 0 ||
        bins_bytes.value() > bytes_.size() - kBaseBlockBytes) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"hive bin領域長が不正です").error());
    }
    declared_end_ = kBaseBlockBytes + bins_bytes.value();
    root_cell_ = root_cell.value();

    std::size_t bin_offset = kBaseBlockBytes;
    while (bin_offset < declared_end_) {
      if (!has_bytes(bytes_, bin_offset, kHiveBinHeaderBytes) ||
          !has_signature(bytes_, bin_offset, 'h', 'b') ||
          !has_signature(bytes_, bin_offset + 2, 'i', 'n')) {
        return clonecore::Status::failure(
            invalid_hive<int>(L"hbinヘッダーが不正です").error());
      }
      const auto relative = read_u32(bytes_, bin_offset + 4);
      const auto bin_size = read_u32(bytes_, bin_offset + 8);
      if (!relative || !bin_size) {
        return clonecore::Status::failure(
            invalid_hive<int>(L"hbinヘッダーが不完全です").error());
      }
      if (relative.value() != bin_offset - kBaseBlockBytes ||
          bin_size.value() < kBaseBlockBytes ||
          bin_size.value() % kBaseBlockBytes != 0 ||
          bin_size.value() > declared_end_ - bin_offset) {
        return clonecore::Status::failure(
            invalid_hive<int>(L"hbin位置または長さが不正です").error());
      }

      const std::size_t bin_end = bin_offset + bin_size.value();
      std::size_t cell_offset = bin_offset + kHiveBinHeaderBytes;
      while (cell_offset < bin_end) {
        const auto signed_size = read_i32(bytes_, cell_offset);
        if (!signed_size || signed_size.value() == 0 ||
            signed_size.value() ==
                (std::numeric_limits<std::int32_t>::min)()) {
          return clonecore::Status::failure(
              invalid_hive<int>(L"hive cell長が不正です").error());
        }
        const std::size_t cell_size = static_cast<std::size_t>(
            signed_size.value() < 0
                ? -static_cast<std::int64_t>(signed_size.value())
                : signed_size.value());
        if (cell_size < 8 || cell_size % 8 != 0 ||
            cell_size > bin_end - cell_offset) {
          return clonecore::Status::failure(
              invalid_hive<int>(L"hive cellがbin範囲外です").error());
        }
        if (signed_size.value() < 0) {
          const std::size_t relative_cell = cell_offset - kBaseBlockBytes;
          if (relative_cell >
              (std::numeric_limits<std::uint32_t>::max)()) {
            return clonecore::Status::failure(invalid_hive<int>(
                L"hive cell offsetが32ビット範囲外です").error());
          }
          cells_.push_back(CellRecord{
              .relative_offset = static_cast<std::uint32_t>(relative_cell),
              .file_offset = cell_offset,
              .size = cell_size,
          });
        }
        cell_offset += cell_size;
      }
      if (cell_offset != bin_end) {
        return clonecore::Status::failure(
            invalid_hive<int>(L"hive cell列がbin終端と一致しません").error());
      }
      bin_offset = bin_end;
    }
    if (bin_offset != declared_end_ || cells_.empty()) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"hive bin列が不完全です").error());
    }
    const auto root = cell(root_cell_);
    if (!root) {
      return clonecore::Status::failure(root.error());
    }
    const auto root_node = parse_key(root_cell_);
    if (!root_node) {
      return clonecore::Status::failure(root_node.error());
    }
    return clonecore::success_status();
  }

  clonecore::Result<WindowsCurrentVersionValues> read_current_version() const {
    std::uint32_t current = root_cell_;
    for (const std::wstring_view segment :
         {L"Microsoft", L"Windows NT", L"CurrentVersion"}) {
      const auto child = find_subkey(current, segment);
      if (!child) {
        return clonecore::Result<WindowsCurrentVersionValues>::failure(
            child.error());
      }
      current = child.value();
    }

    const auto major_value = find_value(current, L"CurrentMajorVersionNumber");
    const auto build_value = find_value(current, L"CurrentBuildNumber");
    const auto type_value = find_value(current, L"InstallationType");
    if (!major_value) {
      return clonecore::Result<WindowsCurrentVersionValues>::failure(
          major_value.error());
    }
    if (!build_value) {
      return clonecore::Result<WindowsCurrentVersionValues>::failure(
          build_value.error());
    }
    if (!type_value) {
      return clonecore::Result<WindowsCurrentVersionValues>::failure(
          type_value.error());
    }
    const auto major = decode_dword(major_value.value());
    const auto build = decode_text(build_value.value());
    const auto installation_type = decode_text(type_value.value());
    if (!major) {
      return clonecore::Result<WindowsCurrentVersionValues>::failure(
          major.error());
    }
    if (!build) {
      return clonecore::Result<WindowsCurrentVersionValues>::failure(
          build.error());
    }
    if (!installation_type) {
      return clonecore::Result<WindowsCurrentVersionValues>::failure(
          installation_type.error());
    }
    return clonecore::Result<WindowsCurrentVersionValues>::success(
        WindowsCurrentVersionValues{
            .major = major.value(),
            .build = build.value(),
            .installation_type = installation_type.value(),
        });
  }

 private:
  clonecore::Result<std::span<const std::byte>> cell(
      const std::uint32_t relative_offset) const {
    const auto found = std::lower_bound(
        cells_.begin(),
        cells_.end(),
        relative_offset,
        [](const CellRecord& record, const std::uint32_t value) {
          return record.relative_offset < value;
        });
    if (found == cells_.end() ||
        found->relative_offset != relative_offset ||
        !has_bytes(bytes_, found->file_offset, found->size)) {
      return invalid_hive<std::span<const std::byte>>(
          L"参照先cellが割当済みcellではありません");
    }
    return clonecore::Result<std::span<const std::byte>>::success(
        bytes_.subspan(found->file_offset, found->size));
  }

  clonecore::Result<std::wstring> decode_name(
      const std::span<const std::byte> bytes,
      const std::size_t offset,
      const std::size_t length,
      const bool compressed,
      const std::size_t maximum) const {
    if (length > maximum || !has_bytes(bytes, offset, length)) {
      return invalid_hive<std::wstring>(L"レジストリ名が範囲外です");
    }
    if (!compressed && length % sizeof(wchar_t) != 0) {
      return invalid_hive<std::wstring>(
          L"Unicodeレジストリ名の長さが不正です");
    }
    std::wstring name;
    try {
      name.reserve(compressed ? length : length / sizeof(wchar_t));
    } catch (const std::bad_alloc&) {
      return clonecore::Result<std::wstring>::failure(hive_error(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"オフラインWindows SOFTWARE hive解析",
          L"レジストリ名を検証するメモリを確保できません"));
    }
    if (compressed) {
      for (std::size_t index = 0; index < length; ++index) {
        name.push_back(static_cast<wchar_t>(
            std::to_integer<std::uint8_t>(bytes[offset + index])));
      }
    } else {
      for (std::size_t index = 0; index < length; index += 2) {
        const auto character = read_u16(bytes, offset + index);
        if (!character) {
          return clonecore::Result<std::wstring>::failure(character.error());
        }
        name.push_back(static_cast<wchar_t>(character.value()));
      }
    }
    if (name.find(L'\0') != std::wstring::npos) {
      return invalid_hive<std::wstring>(
          L"レジストリ名にNULが含まれています");
    }
    return clonecore::Result<std::wstring>::success(std::move(name));
  }

  clonecore::Result<KeyNode> parse_key(
      const std::uint32_t key_offset) const {
    const auto key_cell = cell(key_offset);
    if (!key_cell) {
      return clonecore::Result<KeyNode>::failure(key_cell.error());
    }
    const auto bytes = key_cell.value();
    if (bytes.size() < 80 || !has_signature(bytes, 4, 'n', 'k')) {
      return invalid_hive<KeyNode>(L"nk key cellが不正です");
    }
    const auto flags = read_u16(bytes, 6);
    const auto subkey_count = read_u32(bytes, 24);
    const auto subkey_list = read_u32(bytes, 32);
    const auto value_count = read_u32(bytes, 40);
    const auto value_list = read_u32(bytes, 44);
    const auto name_length = read_u16(bytes, 76);
    if (!flags || !subkey_count || !subkey_list || !value_count ||
        !value_list || !name_length) {
      return invalid_hive<KeyNode>(L"nk key cellが不完全です");
    }
    if (subkey_count.value() > kMaximumSubkeys ||
        value_count.value() > kMaximumValues) {
      return invalid_hive<KeyNode>(L"keyの要素数が安全上限を超えています");
    }
    const auto name = decode_name(
        bytes,
        80,
        name_length.value(),
        (flags.value() & kKeyCompressedName) != 0,
        kMaximumKeyNameBytes);
    if (!name) {
      return clonecore::Result<KeyNode>::failure(name.error());
    }
    return clonecore::Result<KeyNode>::success(KeyNode{
        .subkey_count = subkey_count.value(),
        .subkey_list = subkey_list.value(),
        .value_count = value_count.value(),
        .value_list = value_list.value(),
        .name = name.value(),
    });
  }

  clonecore::Status collect_subkeys(
      const std::uint32_t list_offset,
      const std::size_t depth,
      std::vector<std::uint32_t>& visited,
      std::vector<std::uint32_t>& keys,
      const std::size_t expected) const {
    if (depth > kMaximumIndexDepth || list_offset == kNoCell ||
        std::find(visited.begin(), visited.end(), list_offset) !=
            visited.end()) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"subkey indexが循環または深すぎます").error());
    }
    visited.push_back(list_offset);
    const auto list_cell = cell(list_offset);
    if (!list_cell) {
      return clonecore::Status::failure(list_cell.error());
    }
    const auto bytes = list_cell.value();
    if (bytes.size() < 8) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"subkey index cellが短すぎます").error());
    }
    const auto count = read_u16(bytes, 6);
    if (!count) {
      return clonecore::Status::failure(count.error());
    }
    const bool direct = has_signature(bytes, 4, 'l', 'i');
    const bool hashed = has_signature(bytes, 4, 'l', 'f') ||
        has_signature(bytes, 4, 'l', 'h');
    const bool indirect = has_signature(bytes, 4, 'r', 'i');
    if (!direct && !hashed && !indirect) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"未知のsubkey index形式です").error());
    }
    const std::size_t stride = hashed ? 8U : 4U;
    const std::size_t count_value = count.value();
    if (count_value > expected ||
        count_value > (bytes.size() - 8U) / stride) {
      return clonecore::Status::failure(
          invalid_hive<int>(L"subkey index件数がcell範囲外です").error());
    }
    for (std::size_t index = 0; index < count_value; ++index) {
      const auto entry = read_u32(bytes, 8U + index * stride);
      if (!entry) {
        return clonecore::Status::failure(entry.error());
      }
      if (indirect) {
        const auto status = collect_subkeys(
            entry.value(), depth + 1U, visited, keys, expected);
        if (!status) {
          return status;
        }
      } else {
        if (keys.size() >= expected ||
            std::find(keys.begin(), keys.end(), entry.value()) != keys.end()) {
          return clonecore::Status::failure(
              invalid_hive<int>(L"subkey indexに重複があります").error());
        }
        keys.push_back(entry.value());
      }
    }
    return clonecore::success_status();
  }

  clonecore::Result<std::uint32_t> find_subkey(
      const std::uint32_t parent_offset,
      const std::wstring_view expected_name) const {
    const auto parent = parse_key(parent_offset);
    if (!parent) {
      return clonecore::Result<std::uint32_t>::failure(parent.error());
    }
    if (parent.value().subkey_count == 0 ||
        parent.value().subkey_list == kNoCell) {
      return invalid_hive<std::uint32_t>(L"必須subkeyがありません");
    }
    std::vector<std::uint32_t> visited;
    std::vector<std::uint32_t> keys;
    try {
      keys.reserve(parent.value().subkey_count);
    } catch (const std::bad_alloc&) {
      return clonecore::Result<std::uint32_t>::failure(hive_error(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"オフラインWindows SOFTWARE hive解析",
          L"subkey検証メモリを確保できません"));
    }
    const auto status = collect_subkeys(
        parent.value().subkey_list,
        0,
        visited,
        keys,
        parent.value().subkey_count);
    if (!status) {
      return clonecore::Result<std::uint32_t>::failure(status.error());
    }
    if (keys.size() != parent.value().subkey_count) {
      return invalid_hive<std::uint32_t>(
          L"subkey件数がnk宣言と一致しません");
    }
    std::uint32_t match{kNoCell};
    for (const auto key_offset : keys) {
      const auto key = parse_key(key_offset);
      if (!key) {
        return clonecore::Result<std::uint32_t>::failure(key.error());
      }
      if (_wcsicmp(
              key.value().name.c_str(),
              std::wstring(expected_name).c_str()) == 0) {
        if (match != kNoCell) {
          return invalid_hive<std::uint32_t>(
              L"必須subkeyが重複しています");
        }
        match = key_offset;
      }
    }
    if (match == kNoCell) {
      return invalid_hive<std::uint32_t>(L"必須subkeyが見つかりません");
    }
    return clonecore::Result<std::uint32_t>::success(match);
  }

  clonecore::Result<RegistryValue> parse_value(
      const std::uint32_t value_offset,
      const std::wstring_view expected_name,
      bool& matched) const {
    matched = false;
    const auto value_cell = cell(value_offset);
    if (!value_cell) {
      return clonecore::Result<RegistryValue>::failure(value_cell.error());
    }
    const auto bytes = value_cell.value();
    if (bytes.size() < 24 || !has_signature(bytes, 4, 'v', 'k')) {
      return invalid_hive<RegistryValue>(L"vk value cellが不正です");
    }
    const auto name_length = read_u16(bytes, 6);
    const auto data_length_field = read_u32(bytes, 8);
    const auto data_offset = read_u32(bytes, 12);
    const auto type = read_u32(bytes, 16);
    const auto flags = read_u16(bytes, 20);
    if (!name_length || !data_length_field || !data_offset || !type ||
        !flags) {
      return invalid_hive<RegistryValue>(L"vk value cellが不完全です");
    }
    const auto name = decode_name(
        bytes,
        24,
        name_length.value(),
        (flags.value() & kValueCompressedName) != 0,
        kMaximumValueNameBytes);
    if (!name) {
      return clonecore::Result<RegistryValue>::failure(name.error());
    }
    const std::wstring expected(expected_name);
    if (_wcsicmp(name.value().c_str(), expected.c_str()) != 0) {
      return clonecore::Result<RegistryValue>::success(RegistryValue{});
    }
    matched = true;

    const bool inline_data =
        (data_length_field.value() & 0x80000000U) != 0;
    const std::size_t data_length =
        data_length_field.value() & 0x7FFFFFFFU;
    if (data_length == 0 || data_length > kMaximumVersionValueBytes) {
      return invalid_hive<RegistryValue>(
          L"必須valueのデータ長が許可範囲外です");
    }
    RegistryValue value{.type = type.value()};
    try {
      value.data.resize(data_length);
    } catch (const std::bad_alloc&) {
      return clonecore::Result<RegistryValue>::failure(hive_error(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"オフラインWindows SOFTWARE hive解析",
          L"value検証メモリを確保できません"));
    }
    if (inline_data) {
      if (data_length > sizeof(std::uint32_t) ||
          !has_bytes(bytes, 12, data_length)) {
        return invalid_hive<RegistryValue>(
            L"inline valueデータ長が不正です");
      }
      std::copy_n(bytes.begin() + 12, data_length, value.data.begin());
    } else {
      const auto data_cell = cell(data_offset.value());
      if (!data_cell || data_cell.value().size() < 4U + data_length) {
        return invalid_hive<RegistryValue>(
            L"valueデータcellが範囲外です");
      }
      std::copy_n(
          data_cell.value().begin() + 4,
          data_length,
          value.data.begin());
    }
    return clonecore::Result<RegistryValue>::success(std::move(value));
  }

  clonecore::Result<RegistryValue> find_value(
      const std::uint32_t key_offset,
      const std::wstring_view expected_name) const {
    const auto key = parse_key(key_offset);
    if (!key) {
      return clonecore::Result<RegistryValue>::failure(key.error());
    }
    if (key.value().value_count == 0 || key.value().value_list == kNoCell) {
      return invalid_hive<RegistryValue>(L"必須valueがありません");
    }
    const auto list = cell(key.value().value_list);
    if (!list) {
      return clonecore::Result<RegistryValue>::failure(list.error());
    }
    if (key.value().value_count > (list.value().size() - 4U) / 4U) {
      return invalid_hive<RegistryValue>(L"value listがcell範囲外です");
    }
    bool found{};
    RegistryValue result;
    for (std::size_t index = 0; index < key.value().value_count; ++index) {
      const auto offset = read_u32(list.value(), 4U + index * 4U);
      if (!offset) {
        return clonecore::Result<RegistryValue>::failure(offset.error());
      }
      bool matched{};
      const auto value = parse_value(offset.value(), expected_name, matched);
      if (!value) {
        return clonecore::Result<RegistryValue>::failure(value.error());
      }
      if (matched) {
        if (found) {
          return invalid_hive<RegistryValue>(L"必須valueが重複しています");
        }
        result = value.value();
        found = true;
      }
    }
    if (!found) {
      return invalid_hive<RegistryValue>(L"必須valueが見つかりません");
    }
    return clonecore::Result<RegistryValue>::success(std::move(result));
  }

  clonecore::Result<std::uint32_t> decode_dword(
      const RegistryValue& value) const {
    if (value.type != REG_DWORD || value.data.size() != 4) {
      return invalid_hive<std::uint32_t>(
          L"CurrentMajorVersionNumberの型または長さが不正です");
    }
    return read_u32(value.data, 0);
  }

  clonecore::Result<std::wstring> decode_text(
      const RegistryValue& value) const {
    if ((value.type != REG_SZ && value.type != REG_EXPAND_SZ) ||
        value.data.size() < sizeof(wchar_t) ||
        value.data.size() % sizeof(wchar_t) != 0) {
      return invalid_hive<std::wstring>(
          L"必須文字列valueの型または長さが不正です");
    }
    std::wstring text;
    try {
      text.reserve(value.data.size() / sizeof(wchar_t));
    } catch (const std::bad_alloc&) {
      return clonecore::Result<std::wstring>::failure(hive_error(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"オフラインWindows SOFTWARE hive解析",
          L"文字列value検証メモリを確保できません"));
    }
    for (std::size_t offset = 0; offset < value.data.size(); offset += 2) {
      const auto character = read_u16(value.data, offset);
      if (!character) {
        return clonecore::Result<std::wstring>::failure(character.error());
      }
      text.push_back(static_cast<wchar_t>(character.value()));
    }
    while (!text.empty() && text.back() == L'\0') {
      text.pop_back();
    }
    if (text.empty() || text.find(L'\0') != std::wstring::npos) {
      return invalid_hive<std::wstring>(
          L"必須文字列valueが空または途中で終端されています");
    }
    return clonecore::Result<std::wstring>::success(std::move(text));
  }

  std::span<const std::byte> bytes_;
  std::vector<CellRecord> cells_;
  std::size_t declared_end_{};
  std::uint32_t root_cell_{kNoCell};
};

}  // namespace

clonecore::Result<WindowsCurrentVersionValues>
read_windows_current_version_from_hive(const std::wstring& hive_path) {
  if (hive_path.empty()) {
    return clonecore::Result<WindowsCurrentVersionValues>::failure(hive_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オフラインWindows SOFTWARE hive解析",
        L"SOFTWARE hiveの絶対パスが必要です"));
  }
  const auto bytes = read_hive_file(hive_path);
  if (!bytes) {
    return clonecore::Result<WindowsCurrentVersionValues>::failure(
        bytes.error());
  }
  HiveParser parser(bytes.value());
  const auto status = parser.validate();
  if (!status) {
    return clonecore::Result<WindowsCurrentVersionValues>::failure(
        status.error());
  }
  return parser.read_current_version();
}

}  // namespace ytec::bootrepair

#include "ytec/windowsapp/adk_patch_archive.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034B50U;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014B50U;
constexpr std::uint32_t kEndSignature = 0x06054B50U;
constexpr std::size_t kEndRecordBytes = 22U;
constexpr std::size_t kCentralHeaderBytes = 46U;
constexpr std::size_t kLocalHeaderBytes = 30U;
constexpr std::size_t kRequiredPatchMembers = 9U;
constexpr std::size_t kMaximumRootNameBytes = 192U;
constexpr std::size_t kDeflateHistoryBytes = 32U * 1024U;
constexpr std::uint64_t kMaximumMemberBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumExpandedBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint16_t kAllowedGeneralPurposeFlags =
    (1U << 1U) | (1U << 2U) | (1U << 3U) | (1U << 11U);

clonecore::Error archive_error(
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
  return clonecore::Result<T>::failure(archive_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status invalid_archive(std::wstring message) {
  return clonecore::Status::failure(archive_error(
      clonecore::ErrorCode::verification_failed,
      ERROR_INVALID_DATA,
      L"ADK更新ZIP構造検証",
      std::move(message)));
}

template <typename Integer>
bool read_le(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    Integer& value) noexcept {
  static_assert(std::is_unsigned_v<Integer>);
  if (offset > bytes.size() || bytes.size() - offset < sizeof(Integer)) {
    return false;
  }
  value = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    value |= static_cast<Integer>(
                 std::to_integer<unsigned int>(bytes[offset + index]))
             << (index * 8U);
  }
  return true;
}

bool checked_range(
    const std::uint64_t offset,
    const std::uint64_t length,
    const std::uint64_t limit) noexcept {
  return offset <= limit && length <= limit - offset;
}

std::string ascii_lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](const char character) {
        if (character >= 'A' && character <= 'Z') {
          return static_cast<char>(character - 'A' + 'a');
        }
        return character;
      });
  return value;
}

bool strict_ascii(const std::wstring_view value, std::string& result) {
  if (value.empty() || value.size() > 255U) {
    return false;
  }
  result.clear();
  result.reserve(value.size());
  for (const wchar_t character : value) {
    if (character < 0x20 || character > 0x7E) {
      return false;
    }
    result.push_back(static_cast<char>(character));
  }
  return true;
}

bool safe_basename(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 255U || value == "." || value == "..") {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x20U && byte <= 0x7EU && character != '/' &&
           character != '\\' && character != ':' && character != '\0';
  });
}

bool safe_root_name(const std::string_view value) noexcept {
  return value.size() <= kMaximumRootNameBytes && safe_basename(value);
}

bool safe_zip_name(const std::span<const std::byte> bytes, std::string& name) {
  if (bytes.empty() || bytes.size() > 512U) {
    return false;
  }
  name.clear();
  name.reserve(bytes.size());
  for (const std::byte raw : bytes) {
    const auto value = std::to_integer<unsigned int>(raw);
    if (value < 0x20U || value > 0x7EU || value == '\\' || value == ':' ||
        value == 0U) {
      return false;
    }
    name.push_back(static_cast<char>(value));
  }
  return true;
}

bool canonical_sha256(const std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'F');
         });
}

struct CentralEntry final {
  std::string name;
  std::uint16_t flags{};
  std::uint16_t method{};
  std::uint32_t crc32{};
  std::uint32_t compressed_bytes{};
  std::uint32_t uncompressed_bytes{};
  std::uint32_t local_offset{};
  std::uint32_t external_attributes{};
  bool directory{};
};

bool regular_entry_attributes(
    const std::uint32_t external_attributes,
    const bool directory) noexcept {
  constexpr std::uint32_t kUnixTypeMask = 0170000U;
  constexpr std::uint32_t kUnixRegular = 0100000U;
  constexpr std::uint32_t kUnixDirectory = 0040000U;
  constexpr std::uint32_t kDosDirectory = 0x10U;
  const std::uint32_t unix_type =
      (external_attributes >> 16U) & kUnixTypeMask;
  if (unix_type != 0U &&
      unix_type != (directory ? kUnixDirectory : kUnixRegular)) {
    return false;
  }
  const bool dos_directory =
      (external_attributes & kDosDirectory) != 0U;
  return directory ? (unix_type == kUnixDirectory || dos_directory)
                   : !dos_directory;
}

class BitReader final {
 public:
  explicit BitReader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool read(const unsigned int count, std::uint32_t& value) noexcept {
    if (count > 24U || !fill(count)) {
      return false;
    }
    const std::uint64_t mask = count == 0U ? 0U : ((1ULL << count) - 1ULL);
    value = static_cast<std::uint32_t>(bits_ & mask);
    bits_ >>= count;
    available_ -= count;
    consumed_bits_ += count;
    return true;
  }

  bool peek_padded(
      const unsigned int desired,
      std::uint32_t& value,
      unsigned int& available) noexcept {
    if (desired > 24U) {
      return false;
    }
    (void)fill(desired);
    if (available_ == 0U) {
      return false;
    }
    const std::uint64_t mask = (1ULL << desired) - 1ULL;
    value = static_cast<std::uint32_t>(bits_ & mask);
    available = available_;
    return true;
  }

  bool consume(const unsigned int count) noexcept {
    std::uint32_t unused{};
    return read(count, unused);
  }

  bool align_to_byte() noexcept {
    const unsigned int remainder =
        static_cast<unsigned int>(consumed_bits_ & 7ULL);
    return remainder == 0U || consume(8U - remainder);
  }

  bool read_aligned_byte(std::uint8_t& value) noexcept {
    if ((consumed_bits_ & 7ULL) != 0U) {
      return false;
    }
    std::uint32_t raw{};
    if (!read(8U, raw)) {
      return false;
    }
    value = static_cast<std::uint8_t>(raw);
    return true;
  }

  [[nodiscard]] std::uint64_t consumed_bits() const noexcept {
    return consumed_bits_;
  }

 private:
  bool fill(const unsigned int desired) noexcept {
    while (available_ < desired && next_ < bytes_.size()) {
      bits_ |= static_cast<std::uint64_t>(
                   std::to_integer<unsigned int>(bytes_[next_++]))
               << available_;
      available_ += 8U;
    }
    return available_ >= desired;
  }

  std::span<const std::byte> bytes_;
  std::size_t next_{};
  std::uint64_t bits_{};
  unsigned int available_{};
  std::uint64_t consumed_bits_{};
};

struct HuffmanCell final {
  std::uint16_t symbol{};
  std::uint8_t bit_count{};
};

class HuffmanTable final {
 public:
  bool build(
      const std::span<const std::uint8_t> lengths,
      const unsigned int maximum_bits) {
    if (maximum_bits == 0U || maximum_bits > 15U || lengths.empty()) {
      return false;
    }
    maximum_bits_ = maximum_bits;
    table_.assign(1ULL << maximum_bits_, HuffmanCell{});
    std::array<std::uint32_t, 16U> counts{};
    for (const std::uint8_t length : lengths) {
      if (length > maximum_bits_) {
        return false;
      }
      if (length != 0U) {
        ++counts[length];
      }
    }
    int remaining = 1;
    std::size_t symbols{};
    for (unsigned int bits = 1U; bits <= maximum_bits_; ++bits) {
      remaining = (remaining << 1) - static_cast<int>(counts[bits]);
      if (remaining < 0) {
        return false;
      }
      symbols += counts[bits];
    }
    if (symbols == 0U) {
      return false;
    }
    std::array<std::uint32_t, 16U> next_code{};
    std::uint32_t code{};
    for (unsigned int bits = 1U; bits <= maximum_bits_; ++bits) {
      code = (code + counts[bits - 1U]) << 1U;
      next_code[bits] = code;
    }
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
      const unsigned int length = lengths[symbol];
      if (length == 0U) {
        continue;
      }
      const std::uint32_t canonical = next_code[length]++;
      std::uint32_t reversed{};
      for (unsigned int bit = 0; bit < length; ++bit) {
        reversed = (reversed << 1U) | ((canonical >> bit) & 1U);
      }
      const std::uint32_t variants = 1U << (maximum_bits_ - length);
      for (std::uint32_t suffix = 0; suffix < variants; ++suffix) {
        const std::uint32_t index = reversed | (suffix << length);
        if (table_[index].bit_count != 0U) {
          return false;
        }
        table_[index] = HuffmanCell{
            .symbol = static_cast<std::uint16_t>(symbol),
            .bit_count = static_cast<std::uint8_t>(length),
        };
      }
    }
    return true;
  }

  bool decode(BitReader& reader, std::uint16_t& symbol) const noexcept {
    std::uint32_t bits{};
    unsigned int available{};
    if (table_.empty() ||
        !reader.peek_padded(maximum_bits_, bits, available)) {
      return false;
    }
    const HuffmanCell cell = table_[bits];
    if (cell.bit_count == 0U || cell.bit_count > available ||
        !reader.consume(cell.bit_count)) {
      return false;
    }
    symbol = cell.symbol;
    return true;
  }

 private:
  std::vector<HuffmanCell> table_;
  unsigned int maximum_bits_{};
};

std::uint32_t crc32_update(
    std::uint32_t crc,
    const std::uint8_t byte) noexcept {
  static const std::array<std::uint32_t, 256U> table = [] {
    std::array<std::uint32_t, 256U> values{};
    std::uint32_t index = 0;
    for (std::uint32_t& destination : values) {
      std::uint32_t value = index;
      for (unsigned int bit = 0; bit < 8U; ++bit) {
        value = (value >> 1U) ^
                (0xEDB88320U & static_cast<std::uint32_t>(
                                     -static_cast<std::int32_t>(value & 1U)));
      }
      destination = value;
      ++index;
    }
    return values;
  }();
  return (crc >> 8U) ^ table[(crc ^ byte) & 0xFFU];
}

class BoundedOutput final {
 public:
  BoundedOutput(
      const std::uint64_t maximum_bytes,
      const AdkPatchChunkWriter& writer)
      : maximum_bytes_(maximum_bytes),
        writer_(&writer),
        history_(kDeflateHistoryBytes) {
    pending_.reserve(64U * 1024U);
  }

  clonecore::Status push(const std::uint8_t value) {
    if (total_ >= maximum_bytes_) {
      return invalid_archive(L"展開結果が固定MSP長を超えました");
    }
    history_[history_position_] = value;
    history_position_ = (history_position_ + 1U) % history_.size();
    pending_.push_back(static_cast<std::byte>(value));
    crc_ = crc32_update(crc_, value);
    ++total_;
    if (pending_.size() == pending_.capacity()) {
      return flush();
    }
    return clonecore::success_status();
  }

  clonecore::Status copy(const std::uint32_t distance, std::uint32_t length) {
    if (distance == 0U || distance > history_.size() || distance > total_) {
      return invalid_archive(L"DEFLATEの後方参照距離が不正です");
    }
    while (length-- != 0U) {
      const std::size_t source =
          (history_position_ + history_.size() - distance) % history_.size();
      const auto status = push(history_[source]);
      if (!status) {
        return status;
      }
    }
    return clonecore::success_status();
  }

  clonecore::Status finish(const std::uint32_t expected_crc32) {
    if (total_ != maximum_bytes_) {
      return invalid_archive(L"展開結果の長さが固定MSP長と一致しません");
    }
    const auto status = flush();
    if (!status) {
      return status;
    }
    if ((crc_ ^ 0xFFFFFFFFU) != expected_crc32) {
      return clonecore::Status::failure(archive_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"ADK更新ZIP CRC-32検証",
          L"展開したMSPのCRC-32が中央ディレクトリと一致しません"));
    }
    return clonecore::success_status();
  }

 private:
  clonecore::Status flush() {
    if (pending_.empty()) {
      return clonecore::success_status();
    }
    const auto status = (*writer_)(pending_);
    if (!status) {
      return status;
    }
    pending_.clear();
    return clonecore::success_status();
  }

  std::uint64_t maximum_bytes_{};
  const AdkPatchChunkWriter* writer_{};
  std::vector<std::uint8_t> history_;
  std::size_t history_position_{};
  std::vector<std::byte> pending_;
  std::uint64_t total_{};
  std::uint32_t crc_{0xFFFFFFFFU};
};

bool fixed_huffman_tables(HuffmanTable& literals, HuffmanTable& distances) {
  std::array<std::uint8_t, 288U> literal_lengths{};
  std::fill_n(
      literal_lengths.begin(), 144U, static_cast<std::uint8_t>(8U));
  std::fill_n(
      literal_lengths.begin() + 144U,
      112U,
      static_cast<std::uint8_t>(9U));
  std::fill_n(
      literal_lengths.begin() + 256U,
      24U,
      static_cast<std::uint8_t>(7U));
  std::fill_n(
      literal_lengths.begin() + 280U,
      8U,
      static_cast<std::uint8_t>(8U));
  std::array<std::uint8_t, 32U> distance_lengths{};
  distance_lengths.fill(static_cast<std::uint8_t>(5U));
  return literals.build(literal_lengths, 15U) &&
         distances.build(distance_lengths, 15U);
}

bool dynamic_huffman_tables(
    BitReader& reader,
    HuffmanTable& literals,
    HuffmanTable& distances) {
  std::uint32_t raw_hlit{};
  std::uint32_t raw_hdist{};
  std::uint32_t raw_hclen{};
  if (!reader.read(5U, raw_hlit) || !reader.read(5U, raw_hdist) ||
      !reader.read(4U, raw_hclen)) {
    return false;
  }
  const std::size_t literal_count = raw_hlit + 257U;
  const std::size_t distance_count = raw_hdist + 1U;
  const std::size_t code_length_count = raw_hclen + 4U;
  constexpr std::array<std::uint8_t, 19U> order{
      16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
      11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U};
  std::array<std::uint8_t, 19U> code_lengths{};
  for (std::size_t index = 0; index < code_length_count; ++index) {
    std::uint32_t length{};
    if (!reader.read(3U, length)) {
      return false;
    }
    code_lengths[order[index]] = static_cast<std::uint8_t>(length);
  }
  HuffmanTable code_table;
  if (!code_table.build(code_lengths, 7U)) {
    return false;
  }
  std::vector<std::uint8_t> lengths;
  lengths.reserve(literal_count + distance_count);
  while (lengths.size() < literal_count + distance_count) {
    std::uint16_t symbol{};
    if (!code_table.decode(reader, symbol)) {
      return false;
    }
    if (symbol <= 15U) {
      lengths.push_back(static_cast<std::uint8_t>(symbol));
      continue;
    }
    std::uint32_t extra{};
    std::size_t repeat{};
    std::uint8_t value{};
    if (symbol == 16U) {
      if (lengths.empty() || !reader.read(2U, extra)) {
        return false;
      }
      repeat = extra + 3U;
      value = lengths.back();
    } else if (symbol == 17U) {
      if (!reader.read(3U, extra)) {
        return false;
      }
      repeat = extra + 3U;
    } else if (symbol == 18U) {
      if (!reader.read(7U, extra)) {
        return false;
      }
      repeat = extra + 11U;
    } else {
      return false;
    }
    if (repeat > literal_count + distance_count - lengths.size()) {
      return false;
    }
    lengths.insert(lengths.end(), repeat, value);
  }
  if (lengths[256U] == 0U ||
      !literals.build(
          std::span<const std::uint8_t>(lengths.data(), literal_count),
          15U) ||
      !distances.build(
          std::span<const std::uint8_t>(
              lengths.data() + literal_count, distance_count),
          15U)) {
    return false;
  }
  return true;
}

clonecore::Status inflate_codes(
    BitReader& reader,
    const HuffmanTable& literals,
    const HuffmanTable& distances,
    BoundedOutput& output) {
  constexpr std::array<std::uint16_t, 29U> length_base{
      3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 13U,
      15U, 17U, 19U, 23U, 27U, 31U, 35U, 43U, 51U, 59U,
      67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U};
  constexpr std::array<std::uint8_t, 29U> length_extra{
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U,
      1U, 1U, 2U, 2U, 2U, 2U, 3U, 3U, 3U, 3U,
      4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U};
  constexpr std::array<std::uint16_t, 30U> distance_base{
      1U, 2U, 3U, 4U, 5U, 7U, 9U, 13U, 17U, 25U,
      33U, 49U, 65U, 97U, 129U, 193U, 257U, 385U, 513U, 769U,
      1025U, 1537U, 2049U, 3073U, 4097U, 6145U, 8193U, 12289U,
      16385U, 24577U};
  constexpr std::array<std::uint8_t, 30U> distance_extra{
      0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 3U, 3U,
      4U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 8U, 8U,
      9U, 9U, 10U, 10U, 11U, 11U, 12U, 12U, 13U, 13U};
  for (;;) {
    std::uint16_t symbol{};
    if (!literals.decode(reader, symbol)) {
      return invalid_archive(L"DEFLATEリテラル表を復号できません");
    }
    if (symbol < 256U) {
      const auto status = output.push(static_cast<std::uint8_t>(symbol));
      if (!status) {
        return status;
      }
      continue;
    }
    if (symbol == 256U) {
      return clonecore::success_status();
    }
    if (symbol < 257U || symbol > 285U) {
      return invalid_archive(L"DEFLATE予約済み長さコードを拒否しました");
    }
    const std::size_t length_index = symbol - 257U;
    std::uint32_t extra{};
    if (!reader.read(length_extra[length_index], extra)) {
      return invalid_archive(L"DEFLATE長さ追加ビットが途中で切れています");
    }
    const std::uint32_t length = length_base[length_index] + extra;
    std::uint16_t distance_symbol{};
    if (!distances.decode(reader, distance_symbol) || distance_symbol >= 30U ||
        !reader.read(distance_extra[distance_symbol], extra)) {
      return invalid_archive(L"DEFLATE距離コードが不正です");
    }
    const std::uint32_t distance = distance_base[distance_symbol] + extra;
    const auto status = output.copy(distance, length);
    if (!status) {
      return status;
    }
  }
}

clonecore::Status inflate_deflate(
    const std::span<const std::byte> compressed,
    BoundedOutput& output) {
  BitReader reader(compressed);
  bool final_block{};
  while (!final_block) {
    std::uint32_t final{};
    std::uint32_t type{};
    if (!reader.read(1U, final) || !reader.read(2U, type)) {
      return invalid_archive(L"DEFLATEブロックヘッダーが途中で切れています");
    }
    final_block = final != 0U;
    if (type == 0U) {
      if (!reader.align_to_byte()) {
        return invalid_archive(L"DEFLATE無圧縮ブロック境界が不正です");
      }
      std::uint8_t length_low{};
      std::uint8_t length_high{};
      std::uint8_t inverse_low{};
      std::uint8_t inverse_high{};
      if (!reader.read_aligned_byte(length_low) ||
          !reader.read_aligned_byte(length_high) ||
          !reader.read_aligned_byte(inverse_low) ||
          !reader.read_aligned_byte(inverse_high)) {
        return invalid_archive(L"DEFLATE無圧縮ブロック長が途中で切れています");
      }
      const std::uint16_t length = static_cast<std::uint16_t>(
          length_low | (static_cast<std::uint16_t>(length_high) << 8U));
      const std::uint16_t inverse = static_cast<std::uint16_t>(
          inverse_low | (static_cast<std::uint16_t>(inverse_high) << 8U));
      if (static_cast<std::uint16_t>(length ^ 0xFFFFU) != inverse) {
        return invalid_archive(L"DEFLATE無圧縮ブロック長の補数が不正です");
      }
      for (std::uint32_t index = 0; index < length; ++index) {
        std::uint8_t value{};
        if (!reader.read_aligned_byte(value)) {
          return invalid_archive(L"DEFLATE無圧縮データが途中で切れています");
        }
        const auto status = output.push(value);
        if (!status) {
          return status;
        }
      }
    } else if (type == 1U || type == 2U) {
      HuffmanTable literals;
      HuffmanTable distances;
      const bool valid_tables = type == 1U
                                    ? fixed_huffman_tables(literals, distances)
                                    : dynamic_huffman_tables(
                                          reader, literals, distances);
      if (!valid_tables) {
        return invalid_archive(L"DEFLATE Huffman表が不正です");
      }
      const auto status = inflate_codes(reader, literals, distances, output);
      if (!status) {
        return status;
      }
    } else {
      return invalid_archive(L"DEFLATE予約済みブロック形式を拒否しました");
    }
  }
  if ((reader.consumed_bits() + 7ULL) / 8ULL != compressed.size()) {
    return invalid_archive(L"DEFLATEストリーム末尾に未消費データがあります");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<AdkPatchArchiveInspection> inspect_adk_patch_archive(
    const std::span<const std::byte> archive,
    const std::span<const AdkPatchMemberPin> pins) {
  if (pins.size() != kRequiredPatchMembers ||
      archive.size() < kEndRecordBytes ||
      archive.size() > std::numeric_limits<std::uint32_t>::max()) {
    return failure<AdkPatchArchiveInspection>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK更新ZIP固定入力",
        L"固定MSP 9件またはZIP32範囲の入力ではありません");
  }

  std::map<std::string, std::size_t> pin_by_lower_name;
  std::uint64_t pinned_total{};
  for (std::size_t index = 0; index < pins.size(); ++index) {
    std::string archive_name;
    std::string staging_name;
    if (!strict_ascii(pins[index].archive_member_name, archive_name) ||
        !strict_ascii(pins[index].staging_file_name, staging_name) ||
        !safe_basename(archive_name) || !safe_basename(staging_name) ||
        !ascii_lower(archive_name).ends_with(".msp") ||
        !ascii_lower(staging_name).ends_with(".msp") ||
        pins[index].expected_byte_count == 0U ||
        pins[index].expected_byte_count > kMaximumMemberBytes ||
        !canonical_sha256(pins[index].expected_sha256) ||
        !pin_by_lower_name.emplace(ascii_lower(archive_name), index).second ||
        pinned_total > kMaximumExpandedBytes -
                           pins[index].expected_byte_count) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"ADK更新ZIP member固定値",
          L"MSP名、保存名、長さ、SHA-256、重複、または展開上限が不正です");
    }
    pinned_total += pins[index].expected_byte_count;
  }

  const std::size_t end_offset = archive.size() - kEndRecordBytes;
  std::uint32_t signature{};
  std::uint16_t disk_number{};
  std::uint16_t central_disk{};
  std::uint16_t entries_on_disk{};
  std::uint16_t entry_count{};
  std::uint32_t central_bytes{};
  std::uint32_t central_offset{};
  std::uint16_t comment_bytes{};
  if (!read_le(archive, end_offset, signature) ||
      signature != kEndSignature ||
      !read_le(archive, end_offset + 4U, disk_number) ||
      !read_le(archive, end_offset + 6U, central_disk) ||
      !read_le(archive, end_offset + 8U, entries_on_disk) ||
      !read_le(archive, end_offset + 10U, entry_count) ||
      !read_le(archive, end_offset + 12U, central_bytes) ||
      !read_le(archive, end_offset + 16U, central_offset) ||
      !read_le(archive, end_offset + 20U, comment_bytes) ||
      disk_number != 0U || central_disk != 0U ||
      entries_on_disk != entry_count ||
      entry_count != pins.size() + 1U || comment_bytes != 0U ||
      !checked_range(central_offset, central_bytes, end_offset) ||
      static_cast<std::uint64_t>(central_offset) + central_bytes !=
          end_offset) {
    return failure<AdkPatchArchiveInspection>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK更新ZIP終端検証",
        L"単一ディスク、コメントなし、固定10項目のZIP32中央ディレクトリではありません");
  }

  std::vector<CentralEntry> central_entries;
  central_entries.reserve(entry_count);
  std::set<std::string> unique_names;
  std::set<std::uint32_t> unique_local_offsets;
  std::size_t cursor = central_offset;
  for (std::size_t index = 0; index < entry_count; ++index) {
    std::uint16_t version_needed{};
    std::uint16_t flags{};
    std::uint16_t method{};
    std::uint32_t crc32{};
    std::uint32_t compressed_bytes{};
    std::uint32_t uncompressed_bytes{};
    std::uint16_t name_bytes{};
    std::uint16_t extra_bytes{};
    std::uint16_t file_comment_bytes{};
    std::uint16_t disk_start{};
    std::uint32_t external_attributes{};
    std::uint32_t local_offset{};
    if (!checked_range(cursor, kCentralHeaderBytes, end_offset) ||
        !read_le(archive, cursor, signature) ||
        signature != kCentralHeaderSignature ||
        !read_le(archive, cursor + 6U, version_needed) ||
        !read_le(archive, cursor + 8U, flags) ||
        !read_le(archive, cursor + 10U, method) ||
        !read_le(archive, cursor + 16U, crc32) ||
        !read_le(archive, cursor + 20U, compressed_bytes) ||
        !read_le(archive, cursor + 24U, uncompressed_bytes) ||
        !read_le(archive, cursor + 28U, name_bytes) ||
        !read_le(archive, cursor + 30U, extra_bytes) ||
        !read_le(archive, cursor + 32U, file_comment_bytes) ||
        !read_le(archive, cursor + 34U, disk_start) ||
        !read_le(archive, cursor + 38U, external_attributes) ||
        !read_le(archive, cursor + 42U, local_offset)) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIP中央項目検証",
          L"中央ディレクトリ項目が途中で切れているか形式が不正です");
    }
    const std::uint64_t variable_bytes =
        static_cast<std::uint64_t>(name_bytes) + extra_bytes +
        file_comment_bytes;
    if (version_needed > 20U || (flags & ~kAllowedGeneralPurposeFlags) != 0U ||
        (method != 0U && method != 8U) || disk_start != 0U ||
        name_bytes == 0U || extra_bytes > 4096U ||
        file_comment_bytes != 0U ||
        !checked_range(
            cursor + kCentralHeaderBytes, variable_bytes, end_offset)) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_NOT_SUPPORTED,
          L"ADK更新ZIP中央項目境界",
          L"暗号化、Zip64、未知機能、コメント、方式、または長さを拒否しました");
    }
    std::string name;
    if (!safe_zip_name(
            archive.subspan(cursor + kCentralHeaderBytes, name_bytes), name) ||
        !unique_names.insert(ascii_lower(name)).second ||
        !unique_local_offsets.insert(local_offset).second) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_DUP_NAME,
          L"ADK更新ZIP member名検証",
          L"不正名、重複名、大小文字衝突、または重複ローカル項目を拒否しました");
    }
    const bool directory = name.ends_with('/');
    if (!regular_entry_attributes(external_attributes, directory) ||
        (directory && (method != 0U || compressed_bytes != 0U ||
                       uncompressed_bytes != 0U || crc32 != 0U)) ||
        (!directory && (compressed_bytes == 0U ||
                        uncompressed_bytes == 0U))) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIP entry種別検証",
          L"通常ファイルまたは空の通常ディレクトリ以外を拒否しました");
    }
    central_entries.push_back(CentralEntry{
        .name = std::move(name),
        .flags = flags,
        .method = method,
        .crc32 = crc32,
        .compressed_bytes = compressed_bytes,
        .uncompressed_bytes = uncompressed_bytes,
        .local_offset = local_offset,
        .external_attributes = external_attributes,
        .directory = directory,
    });
    cursor += kCentralHeaderBytes + static_cast<std::size_t>(variable_bytes);
  }
  if (cursor != end_offset) {
    return failure<AdkPatchArchiveInspection>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK更新ZIP中央ディレクトリ終端",
        L"中央ディレクトリの長さが終端レコードと一致しません");
  }

  const auto root = std::find_if(
      central_entries.begin(), central_entries.end(),
      [](const CentralEntry& entry) { return entry.directory; });
  if (root == central_entries.end() ||
      std::count_if(
          central_entries.begin(), central_entries.end(),
          [](const CentralEntry& entry) { return entry.directory; }) != 1 ||
      root->name.size() < 2U || root->name.back() != '/' ||
      root->name.find('/') != root->name.size() - 1U ||
      !safe_root_name(
          std::string_view(root->name).substr(0U, root->name.size() - 1U))) {
    return failure<AdkPatchArchiveInspection>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_NAME,
        L"ADK更新ZIPルート検証",
        L"安全な固定ルートディレクトリ1件だけの構成ではありません");
  }

  AdkPatchArchiveInspection inspection{
      .root_directory_name = root->name,
      .entries = std::vector<AdkPatchZipEntry>(pins.size()),
      .total_uncompressed_bytes = pinned_total,
  };
  std::vector<bool> seen_pins(pins.size(), false);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> local_ranges;
  local_ranges.reserve(central_entries.size());
  for (const auto& entry : central_entries) {
    std::uint16_t local_flags{};
    std::uint16_t local_method{};
    std::uint32_t local_crc{};
    std::uint32_t local_compressed{};
    std::uint32_t local_uncompressed{};
    std::uint16_t local_name_bytes{};
    std::uint16_t local_extra_bytes{};
    if (!checked_range(entry.local_offset, kLocalHeaderBytes, central_offset) ||
        !read_le(archive, entry.local_offset, signature) ||
        signature != kLocalHeaderSignature ||
        !read_le(archive, entry.local_offset + 6U, local_flags) ||
        !read_le(archive, entry.local_offset + 8U, local_method) ||
        !read_le(archive, entry.local_offset + 14U, local_crc) ||
        !read_le(archive, entry.local_offset + 18U, local_compressed) ||
        !read_le(archive, entry.local_offset + 22U, local_uncompressed) ||
        !read_le(archive, entry.local_offset + 26U, local_name_bytes) ||
        !read_le(archive, entry.local_offset + 28U, local_extra_bytes) ||
        local_flags != entry.flags || local_method != entry.method ||
        local_name_bytes != entry.name.size() || local_extra_bytes > 4096U) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIPローカル項目検証",
          L"ローカル項目が中央ディレクトリと一致しません");
    }
    if ((entry.flags & (1U << 3U)) == 0U &&
        (local_crc != entry.crc32 ||
         local_compressed != entry.compressed_bytes ||
         local_uncompressed != entry.uncompressed_bytes)) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIPローカル長検証",
          L"ローカル項目のCRCまたは長さが中央固定値と一致しません");
    }
    if ((entry.flags & (1U << 3U)) != 0U &&
        ((local_crc != 0U && local_crc != entry.crc32) ||
         (local_compressed != 0U &&
          local_compressed != entry.compressed_bytes) ||
         (local_uncompressed != 0U &&
          local_uncompressed != entry.uncompressed_bytes))) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIPデータ記述子検証",
          L"データ記述子方式のローカル固定値が中央項目と矛盾します");
    }
    const std::uint64_t data_offset =
        static_cast<std::uint64_t>(entry.local_offset) + kLocalHeaderBytes +
        local_name_bytes + local_extra_bytes;
    if (!checked_range(data_offset, entry.compressed_bytes, central_offset)) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIP圧縮データ境界",
          L"圧縮データが中央ディレクトリ外へはみ出します");
    }
    std::string local_name;
    if (!safe_zip_name(
            archive.subspan(
                entry.local_offset + kLocalHeaderBytes, local_name_bytes),
            local_name) ||
        local_name != entry.name) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_NAME,
          L"ADK更新ZIPローカル名検証",
          L"ローカル項目名が中央ディレクトリと完全一致しません");
    }
    local_ranges.emplace_back(entry.local_offset, data_offset + entry.compressed_bytes);
    if (entry.directory) {
      continue;
    }
    if (!entry.name.starts_with(root->name) ||
        entry.name.find('/', root->name.size()) != std::string::npos) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_NAME,
          L"ADK更新ZIP member階層検証",
          L"固定ルート直下のMSP以外を拒否しました");
    }
    const std::string basename = entry.name.substr(root->name.size());
    const auto pin = pin_by_lower_name.find(ascii_lower(basename));
    if (pin == pin_by_lower_name.end()) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_FILE_NOT_FOUND,
          L"ADK更新ZIP未知member",
          L"固定マニフェストにないZIP memberを拒否しました");
    }
    std::string exact_pin_name;
    (void)strict_ascii(pins[pin->second].archive_member_name, exact_pin_name);
    if (basename != exact_pin_name || seen_pins[pin->second] ||
        entry.uncompressed_bytes != pins[pin->second].expected_byte_count) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIP固定member照合",
          L"MSP名、大小文字、重複、または非圧縮長が固定値と一致しません");
    }
    seen_pins[pin->second] = true;
    inspection.entries[pin->second] = AdkPatchZipEntry{
        .pin_index = pin->second,
        .compression_method = entry.method,
        .expected_crc32 = entry.crc32,
        .compressed_offset = data_offset,
        .compressed_byte_count = entry.compressed_bytes,
        .uncompressed_byte_count = entry.uncompressed_bytes,
    };
  }
  if (std::find(seen_pins.begin(), seen_pins.end(), false) !=
      seen_pins.end()) {
    return failure<AdkPatchArchiveInspection>(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_NOT_FOUND,
        L"ADK更新ZIP必須member",
        L"固定MSP 9件のいずれかがありません");
  }
  std::sort(local_ranges.begin(), local_ranges.end());
  for (std::size_t index = 1; index < local_ranges.size(); ++index) {
    if (local_ranges[index].first < local_ranges[index - 1U].second) {
      return failure<AdkPatchArchiveInspection>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK更新ZIPローカル範囲",
          L"ローカル項目または圧縮データの重なりを拒否しました");
    }
  }
  return clonecore::Result<AdkPatchArchiveInspection>::success(
      std::move(inspection));
}

clonecore::Status extract_adk_patch_archive_entry(
    const std::span<const std::byte> archive,
    const AdkPatchZipEntry& entry,
    const AdkPatchChunkWriter& writer) {
  if (!writer || entry.uncompressed_byte_count == 0U ||
      entry.uncompressed_byte_count > kMaximumMemberBytes ||
      entry.compressed_byte_count == 0U ||
      !checked_range(
          entry.compressed_offset, entry.compressed_byte_count, archive.size()) ||
      (entry.compression_method != 0U && entry.compression_method != 8U)) {
    return invalid_archive(L"検査済みMSP entryの展開境界が不正です");
  }
  const auto compressed = archive.subspan(
      static_cast<std::size_t>(entry.compressed_offset),
      static_cast<std::size_t>(entry.compressed_byte_count));
  BoundedOutput output(entry.uncompressed_byte_count, writer);
  if (entry.compression_method == 0U) {
    if (entry.compressed_byte_count != entry.uncompressed_byte_count) {
      return invalid_archive(L"無圧縮ZIP memberの圧縮長と非圧縮長が一致しません");
    }
    for (const std::byte value : compressed) {
      const auto status = output.push(
          static_cast<std::uint8_t>(std::to_integer<unsigned int>(value)));
      if (!status) {
        return status;
      }
    }
  } else {
    const auto status = inflate_deflate(compressed, output);
    if (!status) {
      return status;
    }
  }
  return output.finish(entry.expected_crc32);
}

}  // namespace ytec::windowsapp

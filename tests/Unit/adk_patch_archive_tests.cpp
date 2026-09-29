#include "ytec/windowsapp/adk_patch_archive.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ytec::windowsapp::AdkPatchMemberPin;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

void append_u16(std::vector<std::byte>& output, const std::uint16_t value) {
  output.push_back(static_cast<std::byte>(value & 0xFFU));
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void write_u32(
    std::vector<std::byte>& output,
    const std::size_t offset,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    output[offset + shift / 8U] =
        static_cast<std::byte>((value >> shift) & 0xFFU);
  }
}

void append_ascii(std::vector<std::byte>& output, const std::string_view text) {
  for (const char character : text) {
    output.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
}

std::uint32_t crc32(const std::span<const std::byte> bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::byte raw : bytes) {
    crc ^= std::to_integer<std::uint8_t>(raw);
    for (unsigned int bit = 0U; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^
            (0xEDB88320U & static_cast<std::uint32_t>(
                                 -static_cast<std::int32_t>(crc & 1U)));
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::vector<std::byte> bytes_from_ascii(const std::string_view text) {
  std::vector<std::byte> result;
  append_ascii(result, text);
  return result;
}

std::vector<std::byte> bytes_from_hex(const std::string_view hex) {
  check((hex.size() & 1U) == 0U, "Hex fixture must have pairs");
  std::vector<std::byte> result;
  result.reserve(hex.size() / 2U);
  const auto nibble = [](const char value) -> unsigned int {
    if (value >= '0' && value <= '9') {
      return static_cast<unsigned int>(value - '0');
    }
    return static_cast<unsigned int>(value - 'a' + 10);
  };
  for (std::size_t index = 0; index < hex.size(); index += 2U) {
    result.push_back(static_cast<std::byte>(
        (nibble(hex[index]) << 4U) | nibble(hex[index + 1U])));
  }
  return result;
}

struct MemberFixture final {
  std::string name;
  std::vector<std::byte> uncompressed;
  std::vector<std::byte> compressed;
  std::uint16_t method{};
  std::uint32_t external_attributes{0100644U << 16U};
};

struct CentralFixture final {
  std::string name;
  std::uint32_t local_offset{};
  std::uint16_t method{};
  std::uint32_t crc{};
  std::uint32_t compressed_bytes{};
  std::uint32_t uncompressed_bytes{};
  std::uint32_t external_attributes{};
};

std::vector<std::byte> build_zip(
    const std::vector<MemberFixture>& members) {
  std::vector<std::byte> zip;
  std::vector<CentralFixture> central;
  central.reserve(members.size() + 1U);
  const auto append_local = [&zip, &central](
                                const std::string& name,
                                const std::uint16_t method,
                                const std::uint32_t checksum,
                                const std::span<const std::byte> compressed,
                                const std::uint32_t uncompressed_bytes,
                                const std::uint32_t external_attributes) {
    const auto offset = static_cast<std::uint32_t>(zip.size());
    append_u32(zip, 0x04034B50U);
    append_u16(zip, 20U);
    append_u16(zip, 0U);
    append_u16(zip, method);
    append_u16(zip, 0U);
    append_u16(zip, 0U);
    append_u32(zip, checksum);
    append_u32(zip, static_cast<std::uint32_t>(compressed.size()));
    append_u32(zip, uncompressed_bytes);
    append_u16(zip, static_cast<std::uint16_t>(name.size()));
    append_u16(zip, 0U);
    append_ascii(zip, name);
    zip.insert(zip.end(), compressed.begin(), compressed.end());
    central.push_back(CentralFixture{
        .name = name,
        .local_offset = offset,
        .method = method,
        .crc = checksum,
        .compressed_bytes = static_cast<std::uint32_t>(compressed.size()),
        .uncompressed_bytes = uncompressed_bytes,
        .external_attributes = external_attributes,
    });
  };
  append_local("KB5101684/", 0U, 0U, {}, 0U,
               (0040755U << 16U) | 0x10U);
  for (const auto& member : members) {
    const auto& compressed = member.method == 0U
                                 ? member.uncompressed
                                 : member.compressed;
    append_local(
        "KB5101684/" + member.name,
        member.method,
        crc32(member.uncompressed),
        compressed,
        static_cast<std::uint32_t>(member.uncompressed.size()),
        member.external_attributes);
  }
  const auto central_offset = static_cast<std::uint32_t>(zip.size());
  for (const auto& entry : central) {
    append_u32(zip, 0x02014B50U);
    append_u16(zip, 0x0314U);
    append_u16(zip, 20U);
    append_u16(zip, 0U);
    append_u16(zip, entry.method);
    append_u16(zip, 0U);
    append_u16(zip, 0U);
    append_u32(zip, entry.crc);
    append_u32(zip, entry.compressed_bytes);
    append_u32(zip, entry.uncompressed_bytes);
    append_u16(zip, static_cast<std::uint16_t>(entry.name.size()));
    append_u16(zip, 0U);
    append_u16(zip, 0U);
    append_u16(zip, 0U);
    append_u16(zip, 0U);
    append_u32(zip, entry.external_attributes);
    append_u32(zip, entry.local_offset);
    append_ascii(zip, entry.name);
  }
  const auto central_bytes =
      static_cast<std::uint32_t>(zip.size()) - central_offset;
  append_u32(zip, 0x06054B50U);
  append_u16(zip, 0U);
  append_u16(zip, 0U);
  append_u16(zip, static_cast<std::uint16_t>(central.size()));
  append_u16(zip, static_cast<std::uint16_t>(central.size()));
  append_u32(zip, central_bytes);
  append_u32(zip, central_offset);
  append_u16(zip, 0U);
  return zip;
}

std::vector<MemberFixture> ordinary_members() {
  std::vector<MemberFixture> members;
  for (std::size_t index = 0; index < 9U; ++index) {
    members.push_back(MemberFixture{
        .name = "patch-" + std::to_string(index + 1U) + ".msp",
        .uncompressed = bytes_from_ascii(
            "synthetic-patch-" + std::to_string(index + 1U)),
    });
  }
  return members;
}

std::vector<AdkPatchMemberPin> pins_for(
    const std::vector<MemberFixture>& members) {
  std::vector<AdkPatchMemberPin> pins;
  pins.reserve(members.size());
  for (std::size_t index = 0; index < members.size(); ++index) {
    pins.push_back(AdkPatchMemberPin{
        .archive_member_name = std::wstring(
            members[index].name.begin(), members[index].name.end()),
        .staging_file_name =
            L"out-" + std::to_wstring(index + 1U) + L".msp",
        .expected_byte_count = members[index].uncompressed.size(),
        .expected_sha256 = std::string(64U, 'A'),
        .expected_signer_subject = L"synthetic",
        .expected_revision_guid = L"{00000000-0000-0000-0000-000000000000}",
    });
  }
  return pins;
}

std::vector<std::size_t> central_header_offsets(
    const std::vector<std::byte>& zip) {
  std::vector<std::size_t> offsets;
  for (std::size_t index = 0; index + 4U <= zip.size(); ++index) {
    if (zip[index] == std::byte{0x50} && zip[index + 1U] == std::byte{0x4B} &&
        zip[index + 2U] == std::byte{0x01} &&
        zip[index + 3U] == std::byte{0x02}) {
      offsets.push_back(index);
    }
  }
  return offsets;
}

void test_exact_stored_archive_extracts_all_members() {
  const auto members = ordinary_members();
  const auto pins = pins_for(members);
  const auto zip = build_zip(members);
  const auto inspected =
      ytec::windowsapp::inspect_adk_patch_archive(zip, pins);
  check(inspected.has_value(), "Exact synthetic ZIP should inspect");
  check(
      inspected.value().entries.size() == 9U &&
          inspected.value().root_directory_name == "KB5101684/",
      "Inspection should retain the one root and nine pinned entries");
  for (std::size_t index = 0; index < members.size(); ++index) {
    std::vector<std::byte> extracted;
    const auto status = ytec::windowsapp::extract_adk_patch_archive_entry(
        zip,
        inspected.value().entries[index],
        [&extracted](const std::span<const std::byte> chunk) {
          extracted.insert(extracted.end(), chunk.begin(), chunk.end());
          return ytec::clonecore::success_status();
        });
    check(status.has_value(), "Pinned stored member should extract");
    check(
        extracted == members[index].uncompressed,
        "Extracted bytes must match the pinned member");
  }
}

void test_fixed_and_dynamic_deflate_are_bounded() {
  auto members = ordinary_members();
  members[0].uncompressed = bytes_from_ascii("hello hello hello hello hello");
  members[0].compressed = bytes_from_hex("cb48cdc9c957c8c04e0200");
  members[0].method = 8U;
  members[1].uncompressed.assign(1000U, std::byte{'A'});
  const auto suffix = bytes_from_ascii("BCDE");
  for (std::size_t index = 0; index < 1000U; ++index) {
    members[1].uncompressed.insert(
        members[1].uncompressed.end(), suffix.begin(), suffix.end());
  }
  members[1].compressed = bytes_from_hex(
      "edc3070900000800b06cbefe958c21c8068b00becbea515555556f2f");
  members[1].method = 8U;
  const auto pins = pins_for(members);
  const auto zip = build_zip(members);
  const auto inspected =
      ytec::windowsapp::inspect_adk_patch_archive(zip, pins);
  check(inspected.has_value(), "Fixed/dynamic DEFLATE ZIP should inspect");
  for (std::size_t index = 0; index < 2U; ++index) {
    std::vector<std::byte> extracted;
    const auto status = ytec::windowsapp::extract_adk_patch_archive_entry(
        zip,
        inspected.value().entries[index],
        [&extracted](const std::span<const std::byte> chunk) {
          extracted.insert(extracted.end(), chunk.begin(), chunk.end());
          return ytec::clonecore::success_status();
        });
    check(status.has_value(), "Valid raw DEFLATE should extract");
    check(
        extracted == members[index].uncompressed,
        "DEFLATE output must match exactly");
  }
}

void test_traversal_unknown_duplicate_and_size_mismatch_fail() {
  auto members = ordinary_members();
  auto pins = pins_for(members);
  members[0].name = "../evil.msp";
  check(
      !ytec::windowsapp::inspect_adk_patch_archive(build_zip(members), pins),
      "Traversal must fail");

  members = ordinary_members();
  members[0].name = "unknown.msp";
  check(
      !ytec::windowsapp::inspect_adk_patch_archive(build_zip(members), pins),
      "Unknown member must fail");

  members = ordinary_members();
  members[1].name = members[0].name;
  check(
      !ytec::windowsapp::inspect_adk_patch_archive(build_zip(members), pins),
      "Duplicate member must fail");

  members = ordinary_members();
  pins = pins_for(members);
  ++pins[0].expected_byte_count;
  check(
      !ytec::windowsapp::inspect_adk_patch_archive(build_zip(members), pins),
      "Declared expansion mismatch must fail before extraction");
}

void test_nonregular_overlap_and_crc_tamper_fail() {
  auto members = ordinary_members();
  auto pins = pins_for(members);
  members[0].external_attributes = 0120777U << 16U;
  check(
      !ytec::windowsapp::inspect_adk_patch_archive(build_zip(members), pins),
      "A symlink-like Unix entry must fail");

  members = ordinary_members();
  auto zip = build_zip(members);
  const auto central = central_header_offsets(zip);
  check(central.size() == 10U, "Synthetic ZIP should have ten central entries");
  write_u32(zip, central[2U] + 42U, 0U);
  check(
      !ytec::windowsapp::inspect_adk_patch_archive(zip, pins),
      "Overlapping/reused local offsets must fail");

  zip = build_zip(members);
  const auto inspected =
      ytec::windowsapp::inspect_adk_patch_archive(zip, pins);
  check(inspected.has_value(), "Untampered ZIP should inspect");
  const auto data_offset = static_cast<std::size_t>(
      inspected.value().entries[0].compressed_offset);
  zip[data_offset] ^= std::byte{0x01};
  const auto extracted = ytec::windowsapp::extract_adk_patch_archive_entry(
      zip,
      inspected.value().entries[0],
      [](const std::span<const std::byte>) {
        return ytec::clonecore::success_status();
      });
  check(!extracted, "CRC mismatch after inspection must fail");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"exact_stored_archive_extracts_all_members",
       test_exact_stored_archive_extracts_all_members},
      {"fixed_and_dynamic_deflate_are_bounded",
       test_fixed_and_dynamic_deflate_are_bounded},
      {"traversal_unknown_duplicate_and_size_mismatch_fail",
       test_traversal_unknown_duplicate_and_size_mismatch_fail},
      {"nonregular_overlap_and_crc_tamper_fail",
       test_nonregular_overlap_and_crc_tamper_fail},
  };
  try {
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
  } catch (const TestFailure& failure) {
    std::cerr << "FAIL " << failure.message << '\n';
    return 1;
  }
  return 0;
}

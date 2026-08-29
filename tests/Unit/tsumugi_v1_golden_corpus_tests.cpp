#include "ytec/clonecore/gpt.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_manifest.h"
#include "ytec/imageformat/tsumugi_stream.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef YTEC_IMAGE_GOLDEN_DIR
#error YTEC_IMAGE_GOLDEN_DIR must identify the immutable v1 corpus directory
#endif

namespace {

constexpr std::size_t kMaximumGoldenBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kExpectedDiskBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kGptLeadingBytes = 34U * 512U;
constexpr std::size_t kGptTrailingBytes = 33U * 512U;
constexpr std::string_view kGoldenPassword = "Golden-Tsumugi-v1!";

struct GoldenFile final {
  std::string_view name;
  std::size_t decoded_bytes{};
  std::string_view sha256;
};

constexpr std::array<GoldenFile, 17U> kCorpus{{
    {"partition-snapshot-mbr-v1.hex", 640U,
     "62417066d682b0f5c3396199c73a5b60b2743510bf8f74821c7fbdce38ebca4e"},
    {"partition-snapshot-mbr-two-partition-v1.hex", 640U,
     "887ac7e5c760eb156b248cc7b2e0a9c3e69c78e2c245eb57c0c9fc122ccf3cf7"},
    {"partition-snapshot-gpt-512e-v1.hex", 34'496U,
     "8498a93d04130a26e0b09bc5f927910de72847fc0200912be2c59d5993679957"},
    {"manifest-exact-mbr-v1.hex", 1'280U,
     "13749987cf2f9ecf6855fff3faacbbaae94f4f9606bd66b70a0f78a1f2fbdb49"},
    {"manifest-exact-gpt-512e-v1.hex", 35'136U,
     "e22159461fe563f219a0856be75054fb341880fbce4aabc3cfb34d4b3f12e9f1"},
    {"manifest-partition-selection-mbr-v1.hex", 1'664U,
     "565129dceb284a6f35722d28a9cad2c11b6bcb9bd32f5c178984e16780855ad8"},
    {"manifest-shrink-wim-mbr-v1.hex", 1'280U,
     "1b2150c4f2ce6e96ff495a5db917f7b6b39822eb4db0ff73949900a711741848"},
    {"manifest-rescue-mbr-v1.hex", 1'280U,
     "3010ff3761c36e0fafcc675c37781291aeb008dd470c29b92a85133a69fa26c5"},
    {"container-exact-mbr-v1.hex", 2'000U,
     "3009c5925cc413464b93291b4cf0ab157a112bdadfa172a799dfc02622ace981"},
    {"container-exact-gpt-512e-v1.hex", 39'952U,
     "b17b8f65ce6d6af7818183459978017abae5aee6845ae3ddd5a13ba9097be594"},
    {"container-exact-mixed-v1.hex", 6'343U,
     "65ebeaa4712b602b7386ece85e435ef76a6854e571a4ae36e3420821abb5fbee"},
    {"container-partition-selection-mbr-v1.hex", 6'480U,
     "18a25442d7ae911366a114f4c408fdf514a39698516c68cd58bfaae35a23219e"},
    {"container-shrink-wim-mbr-v1.hex", 3'537U,
     "e45922a5d911c9a80b33b8ce36fcfd8809c353473fec047e3842653568c2ab35"},
    {"container-encrypted-exact-mbr-v1.hex", 6'096U,
     "4362ae1bc921d06d08dfbe1a74d957f9f5a0990ab395320e89b254ea77c08fd3"},
    {"container-rescue-loss-map-v1.hex", 2'624U,
     "358be2c68cc1cbd1c84ce5b62deffb62bc266e853c664c480c54a5aff57e389f"},
    {"container-rescue-legacy-no-read-evidence-v1.hex", 2'624U,
     "d07f9acad0f25b258b1fffbef1e4b6ef21baa49f51392f72342bbbeb9cb3f2af"},
    {"container-rescue-lossless-v1.hex", 2'512U,
     "89d197c536acbbbba3b8fa0573a17cd668501db1d513c94b45546110bd876a30"},
}};

constexpr const GoldenFile& kSnapshot = kCorpus[0U];
constexpr const GoldenFile& kTwoPartitionSnapshot = kCorpus[1U];
constexpr const GoldenFile& kGptSnapshot = kCorpus[2U];
constexpr const GoldenFile& kExactManifest = kCorpus[3U];
constexpr const GoldenFile& kGptManifest = kCorpus[4U];
constexpr const GoldenFile& kSelectionManifest = kCorpus[5U];
constexpr const GoldenFile& kShrinkManifest = kCorpus[6U];
constexpr const GoldenFile& kRescueManifest = kCorpus[7U];
constexpr const GoldenFile& kExactContainer = kCorpus[8U];
constexpr const GoldenFile& kGptContainer = kCorpus[9U];
constexpr const GoldenFile& kMixedContainer = kCorpus[10U];
constexpr const GoldenFile& kSelectionContainer = kCorpus[11U];
constexpr const GoldenFile& kShrinkContainer = kCorpus[12U];
constexpr const GoldenFile& kEncryptedContainer = kCorpus[13U];
constexpr const GoldenFile& kRescueLossMapContainer = kCorpus[14U];
constexpr const GoldenFile& kLegacyRescueContainer = kCorpus[15U];
constexpr const GoldenFile& kLosslessRescueContainer = kCorpus[16U];

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

int hex_nibble(const char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

std::filesystem::path golden_directory() {
  return std::filesystem::path(YTEC_IMAGE_GOLDEN_DIR);
}

std::vector<std::byte> load_golden(const GoldenFile& fixture) {
  const std::filesystem::path path = golden_directory() / fixture.name;
  std::ifstream input(path, std::ios::binary);
  check(input.is_open(), "golden fixture must be readable: " + path.string());

  const std::string encoded{
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  check(input.good() || input.eof(), "golden fixture read must complete");

  std::string compact;
  compact.reserve(encoded.size());
  for (const unsigned char value : encoded) {
    if (std::isspace(value) != 0) {
      continue;
    }
    check(hex_nibble(static_cast<char>(value)) >= 0,
          "golden fixture may contain only hexadecimal digits and whitespace");
    compact.push_back(static_cast<char>(value));
  }
  check(compact.size() % 2U == 0U, "golden fixture hex length must be even");
  check(compact.size() / 2U <= kMaximumGoldenBytes,
        "golden fixture must remain within the fixed reader bound");

  std::vector<std::byte> decoded(compact.size() / 2U);
  for (std::size_t index = 0U; index < decoded.size(); ++index) {
    const int high = hex_nibble(compact[index * 2U]);
    const int low = hex_nibble(compact[index * 2U + 1U]);
    decoded[index] = static_cast<std::byte>((high << 4) | low);
  }
  check(decoded.size() == fixture.decoded_bytes,
        "golden fixture decoded length must match the sealed registry");
  return decoded;
}

std::string digest_hex(const ytec::imageformat::Sha256Digest& digest) {
  constexpr std::array<char, 16U> kDigits{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(digest.size() * 2U);
  for (const auto value : digest) {
    const auto number = std::to_integer<unsigned int>(value);
    result.push_back(kDigits[(number >> 4U) & 0x0FU]);
    result.push_back(kDigits[number & 0x0FU]);
  }
  return result;
}

void verify_sealed_digest(
    const GoldenFile& fixture,
    const std::span<const std::byte> bytes) {
  const auto digest = ytec::imageformat::sha256(bytes);
  check(digest.has_value(), "golden fixture SHA-256 must be calculable");
  check(digest_hex(digest.value()) == fixture.sha256,
        "golden fixture SHA-256 must match the sealed registry");
}

std::string fixture_stem(const GoldenFile& fixture) {
  const std::string name(fixture.name);
  constexpr std::string_view suffix = ".hex";
  check(name.ends_with(suffix), "golden fixture must use the .hex suffix");
  return name.substr(0U, name.size() - suffix.size());
}

bool manifest_flag_set(
    const ytec::imageformat::TsumugiManifestFlags value,
    const ytec::imageformat::TsumugiManifestFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool partition_flag_set(
    const ytec::imageformat::TsumugiManifestPartitionFlags value,
    const ytec::imageformat::TsumugiManifestPartitionFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool feature_set(
    const ytec::imageformat::TsumugiHeader& header,
    const ytec::imageformat::TsumugiRequiredFeature feature) noexcept {
  return (header.required_features & static_cast<std::uint32_t>(feature)) != 0U;
}

void check_integrity(const ytec::imageformat::TsumugiInspection& image) {
  check(image.header_hash_verified && image.all_chunks_verified &&
            image.global_hash_verified,
        "sealed container must retain all required integrity evidence");
}

class DeterministicBytes final {
 public:
  explicit DeterministicBytes(const std::uint64_t seed) noexcept
      : state_(seed) {}

  std::byte next_byte() noexcept {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return static_cast<std::byte>(
        (state_ * 0x2545F4914F6CDD1DULL) & 0xFFU);
  }

 private:
  std::uint64_t state_{};
};

std::vector<std::byte> deterministic_bytes(
    const std::size_t size,
    const std::uint64_t seed) {
  DeterministicBytes random(seed);
  std::vector<std::byte> result(size);
  for (auto& value : result) {
    value = random.next_byte();
  }
  return result;
}

void test_corpus_registry_seals_every_fixture() {
  std::ifstream registry(
      golden_directory() / "CORPUS-MANIFEST.txt", std::ios::binary);
  check(registry.is_open(), "golden corpus registry must be readable");

  struct RegistryEntry final {
    std::string sha256;
    std::size_t decoded_bytes{};
  };
  std::map<std::string, RegistryEntry> entries;
  std::string line;
  while (std::getline(registry, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    std::string sha256;
    std::string name;
    std::size_t decoded_bytes = 0U;
    std::string unexpected;
    check(
        static_cast<bool>(fields >> sha256 >> name >> decoded_bytes) &&
            !(fields >> unexpected),
        "every non-comment corpus registry line must be canonical");
    check(entries.emplace(
              name, RegistryEntry{std::move(sha256), decoded_bytes}).second,
          "corpus registry entries must be unique");
  }
  check(registry.eof(), "golden corpus registry read must complete");
  check(entries.size() == kCorpus.size(),
        "corpus registry must contain exactly every sealed fixture");

  std::size_t on_disk_hex_files = 0U;
  for (const auto& item : std::filesystem::directory_iterator(
           golden_directory())) {
    if (item.is_regular_file() && item.path().extension() == ".hex") {
      ++on_disk_hex_files;
    }
  }
  check(on_disk_hex_files == kCorpus.size(),
        "every .hex file must be registered and covered by the test");

  for (const auto& fixture : kCorpus) {
    const auto found = entries.find(fixture_stem(fixture));
    check(found != entries.end() && found->second.sha256 == fixture.sha256 &&
              found->second.decoded_bytes == fixture.decoded_bytes,
          "corpus registry entry must match the compiled sealed expectation");
    const auto bytes = load_golden(fixture);
    verify_sealed_digest(fixture, bytes);
  }
}

void verify_snapshot_fixture(
    const GoldenFile& fixture,
    const bool two_partitions) {
  using namespace ytec::imageformat;
  const auto bytes = load_golden(fixture);
  const auto inspected = inspect_partition_snapshot_v1(bytes);
  check(inspected.has_value(), "sealed v1 partition snapshot must remain readable");
  check(inspected.value().style == PartitionTableStyle::mbr &&
            inspected.value().source_disk_size == kExpectedDiskBytes &&
            inspected.value().logical_sector_size == 512U &&
            inspected.value().regions.size() == 1U &&
            inspected.value().regions.front().disk_offset == 0U &&
            inspected.value().regions.front().data.size() == 512U,
        "sealed partition snapshot geometry must remain unchanged");
  const auto& sector = inspected.value().regions.front().data;
  check(sector[510U] == std::byte{0x55} &&
            sector[511U] == std::byte{0xAA} &&
            sector[446U + 4U] == std::byte{0x07},
        "sealed MBR signature and first NTFS entry must remain unchanged");
  if (two_partitions) {
    check(sector[446U] == std::byte{0x80} &&
              sector[462U] == std::byte{0x00} &&
              sector[462U + 4U] == std::byte{0x07},
          "selection snapshot must retain one active and one data entry");
  }
  const auto rebuilt = build_partition_snapshot_v1(inspected.value());
  check(rebuilt.has_value() && rebuilt.value() == bytes,
        "sealed partition snapshot must retain canonical byte encoding");

  auto tampered = bytes;
  tampered.back() ^= std::byte{0x01};
  check(!inspect_partition_snapshot_v1(tampered).has_value(),
        "tampered sealed partition snapshot must fail closed");
}

void verify_gpt_snapshot_fixture() {
  using namespace ytec::imageformat;
  const auto bytes = load_golden(kGptSnapshot);
  const auto inspected = inspect_partition_snapshot_v1(bytes);
  check(inspected.has_value(),
        "sealed GPT/512e partition snapshot must remain readable");
  check(inspected.value().style == PartitionTableStyle::gpt &&
            inspected.value().source_disk_size == kExpectedDiskBytes &&
            inspected.value().logical_sector_size == 512U &&
            inspected.value().regions.size() == 2U,
        "sealed GPT/512e snapshot geometry must remain unchanged");

  const auto& leading = inspected.value().regions[0U];
  const auto& trailing = inspected.value().regions[1U];
  check(leading.disk_offset == 0U &&
            leading.data.size() == kGptLeadingBytes &&
            trailing.disk_offset == kExpectedDiskBytes - kGptTrailingBytes &&
            trailing.data.size() == kGptTrailingBytes,
        "sealed GPT/512e metadata regions must retain canonical bounds");
  constexpr std::array<std::byte, 8U> kGptSignature{
      std::byte{'E'}, std::byte{'F'}, std::byte{'I'}, std::byte{' '},
      std::byte{'P'}, std::byte{'A'}, std::byte{'R'}, std::byte{'T'}};
  check(leading.data[510U] == std::byte{0x55} &&
            leading.data[511U] == std::byte{0xAA} &&
            leading.data[446U + 4U] == std::byte{0xEE} &&
            std::equal(
                kGptSignature.begin(), kGptSignature.end(),
                leading.data.begin() + 512U) &&
            std::equal(
                kGptSignature.begin(), kGptSignature.end(),
                trailing.data.end() - 512U),
        "sealed GPT/512e snapshot must retain protective MBR and both GPT headers");

  const auto rebuilt = build_partition_snapshot_v1(inspected.value());
  check(rebuilt.has_value() && rebuilt.value() == bytes,
        "sealed GPT/512e snapshot must retain canonical byte encoding");

  auto tampered = bytes;
  tampered.back() ^= std::byte{0x01};
  check(!inspect_partition_snapshot_v1(tampered).has_value(),
        "tampered sealed GPT/512e snapshot must fail closed");
}

void test_partition_snapshot_goldens_are_backward_readable() {
  verify_snapshot_fixture(kSnapshot, false);
  verify_snapshot_fixture(kTwoPartitionSnapshot, true);
  verify_gpt_snapshot_fixture();
}

ytec::imageformat::TsumugiManifest inspect_manifest_fixture(
    const GoldenFile& fixture) {
  using namespace ytec::imageformat;
  const auto bytes = load_golden(fixture);
  const auto inspected = inspect_tsumugi_manifest_v1(bytes);
  check(inspected.has_value(), "sealed v1 manifest must remain readable");
  const auto rebuilt = build_tsumugi_manifest_v1(inspected.value());
  check(rebuilt.has_value() && rebuilt.value() == bytes,
        "sealed manifest must retain canonical byte encoding");

  auto tampered = bytes;
  tampered[92U] = std::byte{0x01};
  check(!inspect_tsumugi_manifest_v1(tampered).has_value(),
        "tampered manifest reserved bytes must fail closed");
  return inspected.value();
}

void check_common_manifest(
    const ytec::imageformat::TsumugiManifest& manifest,
    const std::vector<std::byte>& snapshot,
    const ytec::imageformat::TsumugiManifestPartitionStyle expected_style =
        ytec::imageformat::TsumugiManifestPartitionStyle::mbr) {
  using namespace ytec::imageformat;
  check(manifest.partition_style == expected_style &&
            manifest.source_disk_size == kExpectedDiskBytes &&
            manifest.logical_sector_size == 512U &&
            manifest.physical_sector_size == 4096U &&
            manifest.partition_snapshot == snapshot,
        "sealed manifest geometry and embedded snapshot must remain unchanged");
}

void test_manifest_goldens_preserve_feature_semantics() {
  using namespace ytec::imageformat;
  const auto snapshot = load_golden(kSnapshot);
  const auto two_partition_snapshot = load_golden(kTwoPartitionSnapshot);
  const auto gpt_snapshot = load_golden(kGptSnapshot);

  const auto exact = inspect_manifest_fixture(kExactManifest);
  check_common_manifest(exact, snapshot);
  check(exact.mode == TsumugiManifestMode::exact &&
            exact.partitions.size() == 1U &&
            manifest_flag_set(
                exact.flags, TsumugiManifestFlags::source_contains_windows) &&
            manifest_flag_set(
                exact.flags,
                TsumugiManifestFlags::automatic_surplus_allocation),
        "exact manifest mode and allocation policy must remain unchanged");
  const auto& exact_partition = exact.partitions.front();
  check(exact_partition.source_table_index == 1U &&
            exact_partition.source_partition_number == 1U &&
            exact_partition.role == TsumugiManifestPartitionRole::windows &&
            exact_partition.file_system == TsumugiManifestFileSystem::ntfs &&
            partition_flag_set(
                exact_partition.flags,
                TsumugiManifestPartitionFlags::selected) &&
            partition_flag_set(
                exact_partition.flags,
                TsumugiManifestPartitionFlags::required) &&
            exact_partition.name_utf8 == "Windows" &&
            exact_partition.label_utf8 == "System",
        "exact manifest Windows partition semantics must remain unchanged");

  const auto gpt = inspect_manifest_fixture(kGptManifest);
  check_common_manifest(
      gpt, gpt_snapshot, TsumugiManifestPartitionStyle::gpt);
  check(gpt.mode == TsumugiManifestMode::exact &&
            gpt.partitions.size() == 1U &&
            manifest_flag_set(
                gpt.flags, TsumugiManifestFlags::source_contains_windows) &&
            manifest_flag_set(
                gpt.flags,
                TsumugiManifestFlags::automatic_surplus_allocation),
        "GPT/512e manifest mode and allocation policy must remain unchanged");
  const auto& gpt_partition = gpt.partitions.front();
  check(gpt_partition.source_table_index == 1U &&
            gpt_partition.source_partition_number == 1U &&
            gpt_partition.role == TsumugiManifestPartitionRole::windows &&
            gpt_partition.file_system == TsumugiManifestFileSystem::ntfs &&
            partition_flag_set(
                gpt_partition.flags,
                TsumugiManifestPartitionFlags::selected) &&
            partition_flag_set(
                gpt_partition.flags,
                TsumugiManifestPartitionFlags::required) &&
            partition_flag_set(
                gpt_partition.flags,
                TsumugiManifestPartitionFlags::contains_windows) &&
            gpt_partition.source_offset == kPartitionOffset &&
            gpt_partition.source_size == kPartitionBytes &&
            gpt_partition.minimum_target_bytes == kPartitionBytes &&
            gpt_partition.planned_target_bytes == kPartitionBytes &&
            gpt_partition.payload_logical_offset == kPartitionOffset &&
            gpt_partition.payload_logical_length == kPartitionBytes &&
            gpt_partition.type_id ==
                ytec::clonecore::gpt_type_basic_data().bytes &&
            gpt_partition.unique_id[0U] == std::byte{0x72} &&
            gpt_partition.unique_id[15U] == std::byte{0xA5},
        "GPT/512e manifest must retain its Windows extent and raw GUID bindings");

  const auto selection = inspect_manifest_fixture(kSelectionManifest);
  check_common_manifest(selection, two_partition_snapshot);
  check(selection.mode == TsumugiManifestMode::exact &&
            selection.partitions.size() == 2U &&
            manifest_flag_set(
                selection.flags,
                TsumugiManifestFlags::partition_selection),
        "selection manifest must preserve its two-partition selection contract");
  check(partition_flag_set(
            selection.partitions[0U].flags,
            TsumugiManifestPartitionFlags::selected) &&
            selection.partitions[1U].role ==
                TsumugiManifestPartitionRole::data &&
            selection.partitions[1U].file_system ==
                TsumugiManifestFileSystem::exfat &&
            selection.partitions[1U].flags ==
                TsumugiManifestPartitionFlags::none &&
            selection.partitions[1U].minimum_target_bytes == 0U &&
            selection.partitions[1U].payload_logical_length == 0U,
        "selection manifest must retain the selected Windows and skipped data roles");

  const auto shrink = inspect_manifest_fixture(kShrinkManifest);
  check_common_manifest(shrink, snapshot);
  check(shrink.mode == TsumugiManifestMode::shrink &&
            shrink.partitions.size() == 1U &&
            shrink.partitions.front().payload_encoding ==
                TsumugiManifestPayloadEncoding::microsoft_wim_single_image &&
            shrink.partitions.front().payload_format_version ==
                kTsumugiWimPayloadFormatVersion &&
            shrink.partitions.front().cluster_size == 4096U &&
            shrink.partitions.front().minimum_target_bytes ==
                4ULL * 1024ULL * 1024ULL &&
            shrink.partitions.front().planned_target_bytes ==
                6ULL * 1024ULL * 1024ULL &&
            shrink.partitions.front().payload_logical_length == 1'537U,
        "shrink manifest must retain its versioned single-WIM payload contract");

  const auto rescue = inspect_manifest_fixture(kRescueManifest);
  check_common_manifest(rescue, snapshot);
  check(rescue.mode == TsumugiManifestMode::rescue &&
            rescue.partitions.size() == 1U &&
            !manifest_flag_set(
                rescue.flags,
                TsumugiManifestFlags::automatic_surplus_allocation),
        "rescue manifest must remain distinct from exact allocation policy");
}

ytec::imageformat::TsumugiInspection inspect_container_fixture(
    const GoldenFile& fixture,
    const std::optional<std::string_view> password = std::nullopt) {
  using namespace ytec::imageformat;
  const auto bytes = load_golden(fixture);
  const auto inspected = inspect_tsumugi_v1(bytes, {.password = password});
  check(inspected.has_value(), "sealed v1 container must remain readable");
  check_integrity(inspected.value());
  return inspected.value();
}

void verify_container_tamper_rejected(
    const GoldenFile& fixture,
    const std::optional<std::string_view> password = std::nullopt) {
  using namespace ytec::imageformat;
  auto tampered = load_golden(fixture);
  check(tampered.size() > kTsumugiHeaderSize,
        "container fixture must contain authenticated bytes after its header");
  tampered[kTsumugiHeaderSize] ^= std::byte{0x01};
  check(!inspect_tsumugi_v1(tampered, {.password = password}).has_value(),
        "tampered sealed v1 container must fail closed");
}

void test_minimal_exact_container_remains_backward_readable() {
  using namespace ytec::imageformat;
  const auto manifest = load_golden(kExactManifest);
  const auto image = inspect_container_fixture(kExactContainer);
  check(image.header.major_version == kTsumugiMajorVersion &&
            image.header.minor_version == kTsumugiMinorVersion &&
            image.header.payload_kind == TsumugiPayloadKind::exact_disk &&
            image.header.compression == ImageCompression::none &&
            image.header.required_features == 0U &&
            image.header.source_disk_size == kExpectedDiskBytes &&
            image.manifest == manifest && image.chunks.size() == 1U &&
            image.chunks.front().record.flags ==
                TsumugiChunkFlags::zero_filled &&
            image.chunks.front().plaintext.empty(),
        "minimal exact fixture semantics must remain unchanged");
  verify_container_tamper_rejected(kExactContainer);
}

void test_gpt_exact_container_remains_backward_readable() {
  using namespace ytec::imageformat;
  const auto manifest = load_golden(kGptManifest);
  const auto image = inspect_container_fixture(kGptContainer);
  check(image.header.major_version == kTsumugiMajorVersion &&
            image.header.minor_version == kTsumugiMinorVersion &&
            image.header.payload_kind == TsumugiPayloadKind::exact_disk &&
            image.header.compression == ImageCompression::none &&
            image.header.required_features == 0U &&
            image.header.source_disk_size == kExpectedDiskBytes &&
            image.manifest == manifest && image.chunks.size() == 1U &&
            image.chunks.front().record.logical_offset == kPartitionOffset &&
            image.chunks.front().record.logical_length == 4096U &&
            image.chunks.front().record.flags == TsumugiChunkFlags::none &&
            image.chunks.front().record.compression ==
                ImageCompression::none &&
            image.chunks.front().plaintext ==
                deterministic_bytes(4096U, 0x4750544558414354ULL),
        "GPT/512e exact fixture must retain its fixed manifest and payload bytes");
  verify_container_tamper_rejected(kGptContainer);
}

void test_exact_mixed_container_covers_compression_modes() {
  using namespace ytec::imageformat;
  const auto image = inspect_container_fixture(kMixedContainer);
  check(image.header.payload_kind == TsumugiPayloadKind::exact_disk &&
            image.header.compression == ImageCompression::zstandard &&
            image.header.required_features == 0U &&
            image.chunks.size() == 3U,
        "mixed exact fixture must retain three compression-mode records");
  check(image.chunks[0U].record.logical_offset == kPartitionOffset &&
            image.chunks[0U].record.logical_length == 4096U &&
            image.chunks[0U].record.flags == TsumugiChunkFlags::none &&
            image.chunks[0U].record.compression ==
                ImageCompression::zstandard &&
            image.chunks[0U].plaintext ==
                std::vector<std::byte>(4096U, std::byte{0x5A}),
        "compressible exact bytes must remain a verified Zstandard record");
  check(image.chunks[1U].record.logical_offset == 2ULL * 1024ULL * 1024ULL &&
            image.chunks[1U].record.flags == TsumugiChunkFlags::zero_filled &&
            image.chunks[1U].record.compression == ImageCompression::none &&
            image.chunks[1U].plaintext.empty(),
        "implicit zero range must remain data-free and uncompressed");
  check(image.chunks[2U].record.logical_offset == 3ULL * 1024ULL * 1024ULL &&
            image.chunks[2U].record.flags == TsumugiChunkFlags::none &&
            image.chunks[2U].record.compression == ImageCompression::none &&
            image.chunks[2U].plaintext ==
                deterministic_bytes(4096U, 0x45584143544D4958ULL),
        "noncompressible exact bytes must remain stored without compression");
  verify_container_tamper_rejected(kMixedContainer);
}

void test_partition_selection_container_omits_skipped_payload() {
  using namespace ytec::imageformat;
  const auto manifest = load_golden(kSelectionManifest);
  const auto image = inspect_container_fixture(kSelectionContainer);
  check(image.header.payload_kind == TsumugiPayloadKind::exact_disk &&
            image.header.compression == ImageCompression::none &&
            image.manifest == manifest && image.chunks.size() == 1U &&
            image.chunks.front().record.logical_offset == kPartitionOffset &&
            image.chunks.front().record.logical_length == 4096U &&
            image.chunks.front().plaintext ==
                deterministic_bytes(4096U, 0x53454C4543544544ULL),
        "selection fixture must carry only the selected partition payload");
  verify_container_tamper_rejected(kSelectionContainer);
}

void test_shrink_wim_container_preserves_archive_contract() {
  using namespace ytec::imageformat;
  const auto manifest = load_golden(kShrinkManifest);
  const auto image = inspect_container_fixture(kShrinkContainer);
  auto expected = deterministic_bytes(1'537U, 0x57494D5041594C44ULL);
  constexpr std::array<std::byte, 8U> kSyntheticWimPrefix{
      std::byte{'M'}, std::byte{'S'}, std::byte{'W'}, std::byte{'I'},
      std::byte{'M'}, std::byte{0}, std::byte{0}, std::byte{0}};
  std::copy(kSyntheticWimPrefix.begin(), kSyntheticWimPrefix.end(),
            expected.begin());
  check(image.header.payload_kind == TsumugiPayloadKind::shrink_disk &&
            image.manifest == manifest && image.chunks.size() == 1U &&
            image.chunks.front().record.logical_offset == 0U &&
            image.chunks.front().record.logical_length == 1'537U &&
            image.chunks.front().plaintext == expected,
        "shrink fixture must retain its synthetic single-WIM byte stream");
  verify_container_tamper_rejected(kShrinkContainer);
}

void test_encrypted_container_preserves_fixed_kdf_and_authentication() {
  using namespace ytec::imageformat;
  const auto bytes = load_golden(kEncryptedContainer);
  check(!inspect_tsumugi_v1(bytes).has_value(),
        "encrypted golden metadata must not be exposed without a password");
  check(!inspect_tsumugi_v1(
             bytes, {.password = "Wrong-Golden-Password!"}).has_value(),
        "encrypted golden metadata must reject a wrong password");

  const auto image = inspect_container_fixture(
      kEncryptedContainer, kGoldenPassword);
  std::array<std::byte, 16U> expected_salt{};
  for (std::size_t index = 0U; index < expected_salt.size(); ++index) {
    expected_salt[index] = static_cast<std::byte>(0x30U + index);
  }
  std::array<std::byte, kTsumugiGcmNonceBytes> expected_nonce{};
  for (std::size_t index = 0U; index < expected_nonce.size(); ++index) {
    expected_nonce[index] = static_cast<std::byte>(0xA0U + index);
  }
  check(feature_set(image.header, TsumugiRequiredFeature::encrypted) &&
            image.header.required_features ==
                static_cast<std::uint32_t>(TsumugiRequiredFeature::encrypted) &&
            image.header.argon2.memory_kib == kTsumugiArgon2MemoryKiB &&
            image.header.argon2.iterations == kTsumugiArgon2Iterations &&
            image.header.argon2.parallelism == kTsumugiArgon2Parallelism &&
            image.header.argon2.salt == expected_salt &&
            image.header.base_nonce == expected_nonce &&
            image.metadata_authenticated && image.chunks.size() == 1U &&
            image.chunks.front().plaintext ==
                deterministic_bytes(4096U, 0x454E435259505445ULL),
        "encrypted fixture must retain fixed synthetic KDF, nonce, and plaintext semantics");
  verify_container_tamper_rejected(kEncryptedContainer, kGoldenPassword);
}

void test_rescue_containers_preserve_loss_and_legacy_semantics() {
  using namespace ytec::imageformat;
  const auto rescue_manifest = load_golden(kRescueManifest);
  const auto evidence = TsumugiRescueReadEvidence{
      .forward_attempts = 2U,
      .reverse_attempts = 2U,
      .sector_attempts = 3U,
      .zero_fill_read_back_verified = true,
      .forward_native_error = 23U,
      .reverse_native_error = 27U,
      .sector_native_error = 30U,
  };

  const auto loss_map = inspect_container_fixture(kRescueLossMapContainer);
  check(loss_map.header.payload_kind == TsumugiPayloadKind::rescue_disk &&
            loss_map.manifest == rescue_manifest &&
            feature_set(
                loss_map.header,
                TsumugiRequiredFeature::unreadable_range_map) &&
            feature_set(
                loss_map.header,
                TsumugiRequiredFeature::rescue_read_evidence) &&
            loss_map.chunks.size() == 2U &&
            loss_map.chunks[0U].plaintext ==
                deterministic_bytes(512U, 0x5245534355454F4BULL) &&
            loss_map.chunks[1U].record.flags ==
                TsumugiChunkFlags::unreadable_zero_filled &&
            loss_map.chunks[1U].record.rescue_read_evidence == evidence &&
            loss_map.chunks[1U].plaintext.empty(),
        "rescue loss-map fixture must preserve missing bytes and finite read evidence");

  const auto legacy = inspect_container_fixture(kLegacyRescueContainer);
  check(legacy.header.payload_kind == TsumugiPayloadKind::rescue_disk &&
            feature_set(
                legacy.header,
                TsumugiRequiredFeature::unreadable_range_map) &&
            !feature_set(
                legacy.header,
                TsumugiRequiredFeature::rescue_read_evidence) &&
            legacy.chunks.size() == 2U &&
            legacy.chunks[1U].record.flags ==
                TsumugiChunkFlags::unreadable_zero_filled &&
            !legacy.chunks[1U].record.rescue_read_evidence.has_value(),
        "pre-extension rescue fixture must stay readable without fabricated evidence");

  const auto lossless = inspect_container_fixture(kLosslessRescueContainer);
  check(lossless.header.payload_kind == TsumugiPayloadKind::rescue_disk &&
            lossless.header.required_features == 0U &&
            lossless.chunks.size() == 1U &&
            lossless.chunks.front().record.flags == TsumugiChunkFlags::none &&
            lossless.chunks.front().plaintext ==
                deterministic_bytes(512U, 0x4C4F53534C455353ULL),
        "lossless rescue fixture must not forge a missing-range feature");

  verify_container_tamper_rejected(kRescueLossMapContainer);
  verify_container_tamper_rejected(kLegacyRescueContainer);
  verify_container_tamper_rejected(kLosslessRescueContainer);
}

class FixedContainerFile final {
 public:
  FixedContainerFile(
      const GoldenFile& fixture,
      const std::span<const std::byte> bytes) {
    static std::uint64_t sequence = 0U;
    ++sequence;
    path_ = std::filesystem::temp_directory_path() /
        ("ytec-tsumugi-golden-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(GetTickCount64()) + "-" +
         std::to_string(sequence) + "-" + fixture_stem(fixture) +
         ".tsumugi");
    check(!std::filesystem::exists(path_),
          "owned temporary golden path must not pre-exist");
    std::ofstream output(path_, std::ios::binary | std::ios::out);
    check(output.is_open(), "fixed golden bytes must open a temporary reader input");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(output.good(), "fixed golden bytes must reach the temporary input exactly");
  }

  ~FixedContainerFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  FixedContainerFile(const FixedContainerFile&) = delete;
  FixedContainerFile& operator=(const FixedContainerFile&) = delete;

  [[nodiscard]] std::wstring path() const {
    return path_.wstring();
  }

 private:
  std::filesystem::path path_;
};

void stream_read_fixed_container(
    const GoldenFile& fixture,
    const std::size_t expected_chunks,
    const std::optional<std::string_view> password = std::nullopt) {
  using namespace ytec::imageformat;
  const auto bytes = load_golden(fixture);
  FixedContainerFile file(fixture, bytes);
  std::size_t callbacks = 0U;
  const auto read = read_verified_tsumugi_file_v1(
      TsumugiStreamVerifyRequest{
          .image_path = file.path(),
          .password = password,
          .verification_block_bytes = 1'024U,
      },
      [&](const TsumugiChunkRecord& record,
          const std::span<const std::byte> plaintext) {
        ++callbacks;
        const bool zero = record.flags == TsumugiChunkFlags::zero_filled ||
            record.flags == TsumugiChunkFlags::unreadable_zero_filled;
        check(zero ? plaintext.empty()
                   : plaintext.size() == record.logical_length,
              "stream Reader must deliver exact fixed plaintext semantics");
        return ytec::clonecore::success_status();
      });
  check(read.has_value() && callbacks == expected_chunks &&
            read.value().delivered_chunk_count == expected_chunks &&
            read.value().inspection.header_hash_verified &&
            read.value().inspection.all_chunks_verified &&
            read.value().inspection.global_hash_verified,
        "two-pass stream Reader must accept the sealed fixed container directly");
}

void reject_stream_fixed_tamper_before_callback(
    const GoldenFile& fixture,
    std::vector<std::byte> tampered,
    const std::string_view description) {
  using namespace ytec::imageformat;
  FixedContainerFile file(fixture, tampered);
  std::size_t callbacks = 0U;
  const auto rejected = read_verified_tsumugi_file_v1(
      TsumugiStreamVerifyRequest{
          .image_path = file.path(),
          .verification_block_bytes = 1'024U,
      },
      [&](const TsumugiChunkRecord&, const std::span<const std::byte>) {
        ++callbacks;
        return ytec::clonecore::success_status();
      });
  check(!rejected.has_value() && callbacks == 0U,
        "two-pass stream Reader must reject " + std::string(description) +
            " before callbacks");
}

void test_stream_reader_rejects_late_fixed_tamper_before_callback() {
  using namespace ytec::imageformat;
  const auto inspected = inspect_container_fixture(kMixedContainer);
  const auto& header = inspected.header;

  auto payload_tampered = load_golden(kMixedContainer);
  check(header.data.length > 0U &&
            header.data.offset <= payload_tampered.size() &&
            header.data.length <=
                payload_tampered.size() - header.data.offset,
        "fixed payload region must be non-empty and in bounds");
  const auto payload_tail = static_cast<std::size_t>(
      header.data.offset + header.data.length - 1U);
  payload_tampered[payload_tail] ^= std::byte{0x01};
  reject_stream_fixed_tamper_before_callback(
      kMixedContainer, std::move(payload_tampered),
      "a late fixed payload mutation");

  auto footer_tampered = load_golden(kMixedContainer);
  constexpr std::uint64_t kFooterHashOffset = 16U;
  check(header.footer.length >=
                kFooterHashOffset + Sha256Digest{}.size() &&
            header.footer.offset <= footer_tampered.size() &&
            header.footer.length <=
                footer_tampered.size() - header.footer.offset,
        "fixed footer hash region must be in bounds");
  const auto footer_hash_byte = static_cast<std::size_t>(
      header.footer.offset + kFooterHashOffset);
  footer_tampered[footer_hash_byte] ^= std::byte{0x01};
  reject_stream_fixed_tamper_before_callback(
      kMixedContainer, std::move(footer_tampered),
      "a fixed footer hash mutation");
}

void test_stream_reader_accepts_all_fixed_container_bytes() {
  stream_read_fixed_container(kExactContainer, 1U);
  stream_read_fixed_container(kGptContainer, 1U);
  stream_read_fixed_container(kMixedContainer, 3U);
  stream_read_fixed_container(kSelectionContainer, 1U);
  stream_read_fixed_container(kShrinkContainer, 1U);
  stream_read_fixed_container(kEncryptedContainer, 1U, kGoldenPassword);
  stream_read_fixed_container(kRescueLossMapContainer, 2U);
  stream_read_fixed_container(kLegacyRescueContainer, 2U);
  stream_read_fixed_container(kLosslessRescueContainer, 1U);

}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"corpus_registry_seals_every_fixture",
       test_corpus_registry_seals_every_fixture},
      {"partition_snapshot_goldens_are_backward_readable",
       test_partition_snapshot_goldens_are_backward_readable},
      {"manifest_goldens_preserve_feature_semantics",
       test_manifest_goldens_preserve_feature_semantics},
      {"minimal_exact_container_remains_backward_readable",
       test_minimal_exact_container_remains_backward_readable},
      {"gpt_exact_container_remains_backward_readable",
       test_gpt_exact_container_remains_backward_readable},
      {"exact_mixed_container_covers_compression_modes",
       test_exact_mixed_container_covers_compression_modes},
      {"partition_selection_container_omits_skipped_payload",
       test_partition_selection_container_omits_skipped_payload},
      {"shrink_wim_container_preserves_archive_contract",
       test_shrink_wim_container_preserves_archive_contract},
      {"encrypted_container_preserves_fixed_kdf_and_authentication",
       test_encrypted_container_preserves_fixed_kdf_and_authentication},
      {"rescue_containers_preserve_loss_and_legacy_semantics",
       test_rescue_containers_preserve_loss_and_legacy_semantics},
      {"stream_reader_accepts_all_fixed_container_bytes",
       test_stream_reader_accepts_all_fixed_container_bytes},
      {"stream_reader_rejects_late_fixed_tamper_before_callback",
       test_stream_reader_rejects_late_fixed_tamper_before_callback},
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
  if (failures == 0) {
    std::cout << "PASS ytec-tsumugi-v1-golden-corpus-tests fixtures="
              << kCorpus.size() << '\n';
  }
  return failures == 0 ? 0 : 1;
}

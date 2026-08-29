#include "ytec/clonecore/gpt.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_manifest.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kRandomSeed = 0x5954454346555A5AULL;
constexpr std::size_t kMutationIterations = 4096U;
constexpr std::size_t kMaximumInputBytes = 64U * 1024U;
constexpr std::size_t kMaximumSoakMutations = 100'000'000U;
constexpr std::size_t kBoundarySeedCount = 5U;
constexpr std::size_t kGoldenFixtureCount = 17U;
constexpr std::uint64_t kDiskBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kGoldenLogicalSectorBytes = 512U;
constexpr std::uint32_t kGoldenGptEntryCount = 128U;
constexpr std::uint32_t kGoldenGptEntryBytes = 128U;
constexpr std::uint64_t kGoldenGptEntrySectors =
    (static_cast<std::uint64_t>(kGoldenGptEntryCount) *
         kGoldenGptEntryBytes +
     kGoldenLogicalSectorBytes - 1U) /
    kGoldenLogicalSectorBytes;
constexpr std::uint64_t kGoldenGptLeadingSectors =
    2U + kGoldenGptEntrySectors;
constexpr std::uint64_t kGoldenGptTrailingSectors =
    1U + kGoldenGptEntrySectors;
constexpr std::string_view kGoldenPassword = "Golden-Tsumugi-v1!";
constexpr std::string_view kGoldenEmitConfirmation =
    "I_UNDERSTAND_NEW_V1_FIXTURES_ARE_PERMANENT";

class DeterministicRandom final {
 public:
  explicit DeterministicRandom(const std::uint64_t seed) noexcept
      : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return state_ * 0x2545F4914F6CDD1DULL;
  }

  [[nodiscard]] std::size_t index(const std::size_t upper_bound) noexcept {
    if (upper_bound == 0U) {
      return 0U;
    }
    return static_cast<std::size_t>(next() % upper_bound);
  }

  [[nodiscard]] std::byte byte() noexcept {
    return static_cast<std::byte>(next() & 0xFFU);
  }

 private:
  std::uint64_t state_{};
};

struct FuzzStatistics final {
  std::size_t inputs{};
  std::size_t accepted_tsumugi{};
  std::size_t accepted_exact_tsumugi{};
  std::size_t accepted_shrink_tsumugi{};
  std::size_t accepted_rescue_tsumugi{};
  std::size_t accepted_encrypted_tsumugi{};
  std::size_t accepted_manifests{};
  std::size_t accepted_exact_manifests{};
  std::size_t accepted_shrink_manifests{};
  std::size_t accepted_rescue_manifests{};
  std::size_t accepted_gpt_manifests{};
  std::size_t accepted_snapshots{};
  std::size_t accepted_mbr_snapshots{};
  std::size_t accepted_gpt_snapshots{};
};

std::string to_hex(const std::span<const std::byte> bytes) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto value : bytes) {
    const auto number = std::to_integer<unsigned int>(value);
    result.push_back(kDigits[(number >> 4U) & 0x0FU]);
    result.push_back(kDigits[number & 0x0FU]);
  }
  return result;
}

std::vector<std::byte> make_partition_snapshot_seed() {
  using namespace ytec::imageformat;
  PartitionSnapshot snapshot{
      .style = PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  PartitionTableRegion region;
  region.disk_offset = 0U;
  region.data.assign(512U, std::byte{0});
  region.data[446U + 4U] = std::byte{0x07};
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));

  auto encoded = build_partition_snapshot_v1(snapshot);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build partition snapshot seed");
  }
  return encoded.take_value();
}

std::vector<std::byte> make_manifest_seed(
    const std::vector<std::byte>& partition_snapshot) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = TsumugiManifestFlags::source_contains_windows |
          TsumugiManifestFlags::automatic_surplus_allocation,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-10T00:00:00Z",
      .app_version = "image-fuzz-seed",
      .partition_snapshot = partition_snapshot,
  };
  manifest.source_model_hash[0] = std::byte{0x11};
  manifest.source_serial_hash[0] = std::byte{0x22};
  manifest.source_state_hash[0] = std::byte{0x33};

  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::windows,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          TsumugiManifestPartitionFlags::required |
          TsumugiManifestPartitionFlags::active |
          TsumugiManifestPartitionFlags::contains_windows,
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = 3ULL * 1024ULL * 1024ULL,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = "Windows",
      .label_utf8 = "System",
  };
  partition.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(partition));

  auto encoded = build_tsumugi_manifest_v1(manifest);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build Tsumugi manifest seed");
  }
  return encoded.take_value();
}

std::vector<std::byte> make_tsumugi_seed(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  TsumugiBuildRequest request{
      .payload_kind = TsumugiPayloadKind::exact_disk,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .chunk_size = kImageChunkSize16MiB,
      .compression = ImageCompression::none,
      .manifest = manifest,
  };
  request.image_id[0] = std::byte{0x59};
  request.image_id[1] = std::byte{0x54};
  request.image_id[2] = std::byte{0x45};
  request.image_id[3] = std::byte{0x43};

  TsumugiBuildChunk zero_chunk{
      .logical_offset = 0U,
      .logical_length = 512U,
      .flags = TsumugiChunkFlags::zero_filled,
  };
  request.chunks.push_back(std::move(zero_chunk));

  auto encoded = build_tsumugi_v1(request);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build .tsumugi seed");
  }
  return encoded.take_value();
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  T value{};
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error("golden transform read exceeded its bound");
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error("golden transform write exceeded its bound");
  }
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void refresh_golden_header_hash(std::vector<std::byte>& image) {
  constexpr std::size_t kHeaderHashOffset = 188U;
  const std::size_t hash_bytes = ytec::imageformat::Sha256Digest{}.size();
  if (image.size() < ytec::imageformat::kTsumugiHeaderSize) {
    throw std::runtime_error("golden header is truncated");
  }
  std::fill_n(image.begin() + kHeaderHashOffset, hash_bytes, std::byte{0});
  const auto digest = ytec::imageformat::sha256(
      std::span<const std::byte>(
          image.data(), ytec::imageformat::kTsumugiHeaderSize));
  if (!digest.has_value()) {
    throw std::runtime_error("failed to refresh golden header hash");
  }
  std::copy(
      digest.value().begin(), digest.value().end(),
      image.begin() + kHeaderHashOffset);
}

void refresh_golden_global_hash(std::vector<std::byte>& image) {
  const auto footer_offset = read_little<std::uint64_t>(image, 96U);
  if (footer_offset > image.size() ||
      ytec::imageformat::kTsumugiFooterSize >
          image.size() - static_cast<std::size_t>(footer_offset)) {
    throw std::runtime_error("golden footer is outside the image");
  }
  const auto digest = ytec::imageformat::sha256(
      std::span<const std::byte>(
          image.data(), static_cast<std::size_t>(footer_offset)));
  if (!digest.has_value()) {
    throw std::runtime_error("failed to refresh golden global hash");
  }
  std::copy(
      digest.value().begin(), digest.value().end(),
      image.begin() + static_cast<std::ptrdiff_t>(footer_offset + 16U));
}

std::vector<std::byte> deterministic_bytes(
    const std::size_t size,
    const std::uint64_t seed) {
  DeterministicRandom random(seed);
  std::vector<std::byte> bytes(size);
  for (auto& value : bytes) {
    value = random.byte();
  }
  return bytes;
}

std::vector<std::byte> make_two_partition_snapshot_golden() {
  using namespace ytec::imageformat;
  PartitionSnapshot snapshot{
      .style = PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  PartitionTableRegion region;
  region.data.assign(512U, std::byte{0});
  auto sector = std::span<std::byte>(region.data);
  constexpr std::size_t kFirstEntry = 446U;
  constexpr std::size_t kSecondEntry = kFirstEntry + 16U;
  sector[kFirstEntry + 0U] = std::byte{0x80};
  sector[kFirstEntry + 4U] = std::byte{0x07};
  write_little<std::uint32_t>(sector, kFirstEntry + 8U, 2048U);
  write_little<std::uint32_t>(sector, kFirstEntry + 12U, 8U);
  sector[kSecondEntry + 4U] = std::byte{0x07};
  write_little<std::uint32_t>(sector, kSecondEntry + 8U, 4096U);
  write_little<std::uint32_t>(sector, kSecondEntry + 12U, 8U);
  sector[510U] = std::byte{0x55};
  sector[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  auto encoded = build_partition_snapshot_v1(snapshot);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build two-partition snapshot golden");
  }
  return encoded.take_value();
}

ytec::clonecore::GptGuid golden_guid(const std::uint8_t discriminator) {
  ytec::clonecore::GptGuid result;
  result.bytes[0] = static_cast<std::byte>(discriminator);
  result.bytes[15] = std::byte{0xA5};
  return result;
}

class GoldenGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(
        golden_guid(next_++));
  }

 private:
  std::uint8_t next_{0x71U};
};

std::vector<std::byte> make_gpt_snapshot_golden() {
  using namespace ytec::clonecore;
  using namespace ytec::imageformat;

  constexpr std::uint64_t kSectorCount =
      kDiskBytes / kGoldenLogicalSectorBytes;
  constexpr std::uint64_t kWindowsFirstLba =
      kPartitionOffset / kGoldenLogicalSectorBytes;
  constexpr std::uint64_t kWindowsLastLba =
      kWindowsFirstLba +
      kPartitionBytes / kGoldenLogicalSectorBytes - 1U;
  GptDisk layout{
      .logical_sector_size = kGoldenLogicalSectorBytes,
      .sector_count = kSectorCount,
      .disk_guid = golden_guid(0x41U),
      .first_usable_lba = kGoldenGptLeadingSectors,
      .last_usable_lba =
          kSectorCount - kGoldenGptTrailingSectors - 1U,
      .partition_entry_count = kGoldenGptEntryCount,
      .partition_entry_size = kGoldenGptEntryBytes,
      .partitions = {
          GptPartition{
              .entry_index = 0U,
              .type_guid = gpt_type_basic_data(),
              .unique_guid = golden_guid(0x42U),
              .first_lba = kWindowsFirstLba,
              .last_lba = kWindowsLastLba,
              .attributes = 0U,
              .name = u"Windows",
          },
      },
  };
  GoldenGuidGenerator generator;
  auto plan = make_gpt_write_plan(
      layout, kDiskBytes, kGoldenLogicalSectorBytes, generator);
  if (!plan.has_value()) {
    throw std::runtime_error("failed to build deterministic GPT metadata");
  }

  const std::uint64_t trailing_offset =
      kDiskBytes -
      kGoldenGptTrailingSectors * kGoldenLogicalSectorBytes;
  std::vector<std::byte> leading(
      static_cast<std::size_t>(
          kGoldenGptLeadingSectors * kGoldenLogicalSectorBytes),
      std::byte{0});
  std::vector<std::byte> trailing(
      static_cast<std::size_t>(
          kGoldenGptTrailingSectors * kGoldenLogicalSectorBytes),
      std::byte{0});

  const auto place_write = [](
      const GptMetadataWrite& write,
      const std::uint64_t region_offset,
      std::vector<std::byte>& region) {
    if (write.offset < region_offset) {
      return false;
    }
    const std::uint64_t relative = write.offset - region_offset;
    if (relative > region.size() ||
        write.bytes.size() > region.size() - relative) {
      return false;
    }
    std::copy(
        write.bytes.begin(), write.bytes.end(),
        region.begin() + static_cast<std::ptrdiff_t>(relative));
    return true;
  };
  for (const auto& write : plan.value().writes) {
    if (!place_write(write, 0U, leading) &&
        !place_write(write, trailing_offset, trailing)) {
      throw std::runtime_error(
          "deterministic GPT write escaped its metadata regions");
    }
  }

  PartitionSnapshot snapshot{
      .style = PartitionTableStyle::gpt,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = kGoldenLogicalSectorBytes,
      .regions = {
          PartitionTableRegion{
              .disk_offset = 0U,
              .data = std::move(leading),
          },
          PartitionTableRegion{
              .disk_offset = trailing_offset,
              .data = std::move(trailing),
          },
      },
  };
  auto encoded = build_partition_snapshot_v1(snapshot);
  if (!encoded.has_value()) {
    throw std::runtime_error("failed to build GPT snapshot golden");
  }
  return encoded.take_value();
}

ytec::imageformat::TsumugiManifest make_manifest_base(
    const std::vector<std::byte>& partition_snapshot,
    const ytec::imageformat::TsumugiManifestMode mode,
    const ytec::imageformat::TsumugiManifestPartitionStyle style =
        ytec::imageformat::TsumugiManifestPartitionStyle::mbr) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = mode,
      .partition_style = style,
      .flags = TsumugiManifestFlags::source_contains_windows,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-20T00:00:00Z",
      .app_version = "1.0.0-internal-beta-golden",
      .partition_snapshot = partition_snapshot,
  };
  manifest.source_model_hash[0] = std::byte{0x41};
  manifest.source_serial_hash[0] = std::byte{0x52};
  manifest.source_state_hash[0] = std::byte{0x63};
  return manifest;
}

ytec::imageformat::TsumugiManifestPartition make_windows_partition(
    const std::uint64_t source_offset = kPartitionOffset,
    const std::uint64_t source_size = kPartitionBytes) {
  using namespace ytec::imageformat;
  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::windows,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          TsumugiManifestPartitionFlags::required |
          TsumugiManifestPartitionFlags::active |
          TsumugiManifestPartitionFlags::contains_windows,
      .source_offset = source_offset,
      .source_size = source_size,
      .used_bytes = (std::min)(source_size, 3ULL * 1024ULL * 1024ULL),
      .minimum_target_bytes = source_size,
      .planned_target_bytes = source_size,
      .payload_logical_offset = source_offset,
      .payload_logical_length = source_size,
      .name_utf8 = "Windows",
      .label_utf8 = "Golden System",
  };
  partition.type_id[0] = std::byte{0x07};
  return partition;
}

ytec::imageformat::TsumugiManifestPartition make_gpt_windows_partition() {
  auto partition = make_windows_partition();
  partition.type_id = ytec::clonecore::gpt_type_basic_data().bytes;
  partition.unique_id = golden_guid(0x72U).bytes;
  return partition;
}

std::vector<std::byte> encode_manifest_golden(
    ytec::imageformat::TsumugiManifest manifest,
    const char* const operation) {
  auto encoded = ytec::imageformat::build_tsumugi_manifest_v1(manifest);
  if (!encoded.has_value()) {
    throw std::runtime_error(std::string("failed to build ") + operation);
  }
  return encoded.take_value();
}

std::vector<std::byte> make_partition_selection_manifest_golden(
    const std::vector<std::byte>& snapshot) {
  using namespace ytec::imageformat;
  auto manifest = make_manifest_base(snapshot, TsumugiManifestMode::exact);
  manifest.flags = manifest.flags |
      TsumugiManifestFlags::partition_selection |
      TsumugiManifestFlags::automatic_surplus_allocation;
  manifest.partitions.push_back(make_windows_partition(
      1ULL * 1024ULL * 1024ULL, 4096U));
  TsumugiManifestPartition skipped{
      .source_table_index = 2U,
      .source_partition_number = 2U,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::exfat,
      .flags = TsumugiManifestPartitionFlags::none,
      .source_offset = 2ULL * 1024ULL * 1024ULL,
      .source_size = 4096U,
      .used_bytes = 2048U,
      .name_utf8 = "Data",
      .label_utf8 = "Skipped",
  };
  skipped.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(skipped));
  return encode_manifest_golden(
      std::move(manifest), "partition-selection manifest golden");
}

std::vector<std::byte> make_gpt_exact_manifest_golden(
    const std::vector<std::byte>& snapshot) {
  using namespace ytec::imageformat;
  auto manifest = make_manifest_base(
      snapshot,
      TsumugiManifestMode::exact,
      TsumugiManifestPartitionStyle::gpt);
  manifest.flags = manifest.flags |
      TsumugiManifestFlags::automatic_surplus_allocation;
  manifest.partitions.push_back(make_gpt_windows_partition());
  return encode_manifest_golden(
      std::move(manifest), "GPT exact manifest golden");
}

std::vector<std::byte> make_shrink_wim_manifest_golden(
    const std::vector<std::byte>& snapshot) {
  using namespace ytec::imageformat;
  auto manifest = make_manifest_base(snapshot, TsumugiManifestMode::shrink);
  manifest.flags = manifest.flags |
      TsumugiManifestFlags::automatic_surplus_allocation;
  auto partition = make_windows_partition();
  partition.minimum_target_bytes = 4ULL * 1024ULL * 1024ULL;
  partition.planned_target_bytes = 6ULL * 1024ULL * 1024ULL;
  partition.payload_logical_offset = 0U;
  partition.payload_logical_length = 1537U;
  partition.payload_encoding =
      TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
  partition.payload_format_version = kTsumugiWimPayloadFormatVersion;
  partition.cluster_size = 4096U;
  manifest.partitions.push_back(std::move(partition));
  return encode_manifest_golden(
      std::move(manifest), "shrink-WIM manifest golden");
}

std::vector<std::byte> make_rescue_manifest_golden(
    const std::vector<std::byte>& snapshot) {
  using namespace ytec::imageformat;
  auto manifest = make_manifest_base(snapshot, TsumugiManifestMode::rescue);
  manifest.partitions.push_back(make_windows_partition());
  return encode_manifest_golden(
      std::move(manifest), "rescue manifest golden");
}

std::array<std::byte, 16U> image_id(
    const std::uint8_t discriminator) {
  std::array<std::byte, 16U> result{};
  result[0] = std::byte{'Y'};
  result[1] = std::byte{'T'};
  result[2] = std::byte{'E'};
  result[3] = std::byte{'C'};
  result[15] = static_cast<std::byte>(discriminator);
  return result;
}

std::vector<std::byte> build_container_golden(
    ytec::imageformat::TsumugiBuildRequest request,
    const char* const operation) {
  auto encoded = ytec::imageformat::build_tsumugi_v1(request);
  if (!encoded.has_value()) {
    throw std::runtime_error(std::string("failed to build ") + operation);
  }
  return encoded.take_value();
}

ytec::imageformat::TsumugiBuildRequest make_container_base(
    std::vector<std::byte> manifest,
    const ytec::imageformat::TsumugiPayloadKind kind,
    const std::uint8_t discriminator) {
  return ytec::imageformat::TsumugiBuildRequest{
      .payload_kind = kind,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .chunk_size = ytec::imageformat::kImageChunkSize16MiB,
      .compression = ytec::imageformat::ImageCompression::zstandard,
      .image_id = image_id(discriminator),
      .manifest = std::move(manifest),
  };
}

std::vector<std::byte> make_exact_mixed_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::exact_disk, 0x11U);
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = 1ULL * 1024ULL * 1024ULL,
      .logical_length = 4096U,
      .data = std::vector<std::byte>(4096U, std::byte{0x5A}),
  });
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = 2ULL * 1024ULL * 1024ULL,
      .logical_length = 4096U,
      .flags = TsumugiChunkFlags::zero_filled,
  });
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = 3ULL * 1024ULL * 1024ULL,
      .logical_length = 4096U,
      .data = deterministic_bytes(4096U, 0x45584143544D4958ULL),
  });
  return build_container_golden(
      std::move(request), "mixed exact container golden");
}

std::vector<std::byte> make_selection_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::exact_disk, 0x22U);
  request.compression = ImageCompression::none;
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = 1ULL * 1024ULL * 1024ULL,
      .logical_length = 4096U,
      .data = deterministic_bytes(4096U, 0x53454C4543544544ULL),
  });
  return build_container_golden(
      std::move(request), "partition-selection container golden");
}

std::vector<std::byte> make_gpt_exact_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::exact_disk, 0x77U);
  request.compression = ImageCompression::none;
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = 4096U,
      .data = deterministic_bytes(4096U, 0x4750544558414354ULL),
  });
  return build_container_golden(
      std::move(request), "GPT exact container golden");
}

std::vector<std::byte> make_shrink_wim_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::shrink_disk, 0x33U);
  auto payload = deterministic_bytes(1537U, 0x57494D5041594C44ULL);
  constexpr std::array<std::byte, 8U> kSyntheticWimPrefix{
      std::byte{'M'}, std::byte{'S'}, std::byte{'W'}, std::byte{'I'},
      std::byte{'M'}, std::byte{0}, std::byte{0}, std::byte{0}};
  std::copy(kSyntheticWimPrefix.begin(), kSyntheticWimPrefix.end(),
            payload.begin());
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = 0U,
      .logical_length = payload.size(),
      .data = std::move(payload),
  });
  return build_container_golden(
      std::move(request), "shrink-WIM container golden");
}

std::vector<std::byte> make_encrypted_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::exact_disk, 0x44U);
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = 4096U,
      .data = deterministic_bytes(4096U, 0x454E435259505445ULL),
  });
  TsumugiEncryptionSettings encryption{
      .password = kGoldenPassword,
  };
  for (std::size_t index = 0U; index < encryption.argon2.salt.size();
       ++index) {
    encryption.argon2.salt[index] =
        static_cast<std::byte>(0x30U + index);
  }
  for (std::size_t index = 0U; index < encryption.base_nonce.size();
       ++index) {
    encryption.base_nonce[index] =
        static_cast<std::byte>(0xA0U + index);
  }
  request.encryption = encryption;
  return build_container_golden(
      std::move(request), "encrypted exact container golden");
}

ytec::imageformat::TsumugiRescueReadEvidence golden_rescue_evidence() {
  return ytec::imageformat::TsumugiRescueReadEvidence{
      .forward_attempts = 2U,
      .reverse_attempts = 2U,
      .sector_attempts = 3U,
      .zero_fill_read_back_verified = true,
      .forward_native_error = 23U,
      .reverse_native_error = 27U,
      .sector_native_error = 30U,
  };
}

std::vector<std::byte> make_rescue_loss_map_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::rescue_disk, 0x55U);
  request.compression = ImageCompression::none;
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = 512U,
      .data = deterministic_bytes(512U, 0x5245534355454F4BULL),
  });
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = kPartitionOffset + 512U,
      .logical_length = 512U,
      .flags = TsumugiChunkFlags::unreadable_zero_filled,
      .rescue_read_evidence = golden_rescue_evidence(),
  });
  return build_container_golden(
      std::move(request), "rescue loss-map container golden");
}

std::vector<std::byte> make_legacy_rescue_container_golden(
    const std::vector<std::byte>& current_rescue,
    const std::size_t manifest_size) {
  auto legacy = current_rescue;
  const auto features = read_little<std::uint32_t>(legacy, 20U);
  write_little<std::uint32_t>(
      legacy, 20U,
      features & ~static_cast<std::uint32_t>(
          ytec::imageformat::TsumugiRequiredFeature::rescue_read_evidence));
  const auto metadata_offset = read_little<std::uint64_t>(legacy, 80U);
  const auto second_record_offset =
      static_cast<std::size_t>(metadata_offset) +
      ytec::imageformat::kTsumugiMetadataHeaderSize + manifest_size +
      ytec::imageformat::kTsumugiChunkRecordSize;
  if (second_record_offset > legacy.size() ||
      ytec::imageformat::kTsumugiChunkRecordSize >
          legacy.size() - second_record_offset) {
    throw std::runtime_error("legacy rescue record is outside the image");
  }
  std::fill_n(
      legacy.begin() + static_cast<std::ptrdiff_t>(second_record_offset + 96U),
      16U, std::byte{0});
  refresh_golden_header_hash(legacy);
  refresh_golden_global_hash(legacy);
  return legacy;
}

std::vector<std::byte> make_lossless_rescue_container_golden(
    const std::vector<std::byte>& manifest) {
  using namespace ytec::imageformat;
  auto request = make_container_base(
      manifest, TsumugiPayloadKind::rescue_disk, 0x66U);
  request.chunks.push_back(TsumugiBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = 512U,
      .data = deterministic_bytes(512U, 0x4C4F53534C455353ULL),
  });
  return build_container_golden(
      std::move(request), "lossless rescue container golden");
}

struct GoldenOutput final {
  std::string_view name;
  std::vector<std::byte> bytes;
};

std::vector<GoldenOutput> make_golden_outputs() {
  auto snapshot = make_partition_snapshot_seed();
  auto manifest = make_manifest_seed(snapshot);
  auto base_container = make_tsumugi_seed(manifest);
  auto two_partition_snapshot = make_two_partition_snapshot_golden();
  auto selection_manifest =
      make_partition_selection_manifest_golden(two_partition_snapshot);
  auto gpt_snapshot = make_gpt_snapshot_golden();
  auto gpt_manifest = make_gpt_exact_manifest_golden(gpt_snapshot);
  auto shrink_manifest = make_shrink_wim_manifest_golden(snapshot);
  auto rescue_manifest = make_rescue_manifest_golden(snapshot);
  auto rescue_loss_map =
      make_rescue_loss_map_container_golden(rescue_manifest);

  std::vector<GoldenOutput> outputs;
  outputs.push_back({"partition-snapshot-mbr-v1.hex", std::move(snapshot)});
  outputs.push_back({"manifest-exact-mbr-v1.hex", manifest});
  outputs.push_back({"container-exact-mbr-v1.hex", std::move(base_container)});
  outputs.push_back({"partition-snapshot-gpt-512e-v1.hex", gpt_snapshot});
  outputs.push_back({"manifest-exact-gpt-512e-v1.hex", gpt_manifest});
  outputs.push_back({
      "container-exact-gpt-512e-v1.hex",
      make_gpt_exact_container_golden(gpt_manifest)});
  outputs.push_back({
      "container-exact-mixed-v1.hex",
      make_exact_mixed_container_golden(manifest)});
  outputs.push_back({
      "partition-snapshot-mbr-two-partition-v1.hex",
      std::move(two_partition_snapshot)});
  outputs.push_back({
      "manifest-partition-selection-mbr-v1.hex", selection_manifest});
  outputs.push_back({
      "container-partition-selection-mbr-v1.hex",
      make_selection_container_golden(selection_manifest)});
  outputs.push_back({"manifest-shrink-wim-mbr-v1.hex", shrink_manifest});
  outputs.push_back({
      "container-shrink-wim-mbr-v1.hex",
      make_shrink_wim_container_golden(shrink_manifest)});
  outputs.push_back({
      "container-encrypted-exact-mbr-v1.hex",
      make_encrypted_container_golden(manifest)});
  outputs.push_back({"manifest-rescue-mbr-v1.hex", rescue_manifest});
  outputs.push_back({"container-rescue-loss-map-v1.hex", rescue_loss_map});
  outputs.push_back({
      "container-rescue-legacy-no-read-evidence-v1.hex",
      make_legacy_rescue_container_golden(
          rescue_loss_map, rescue_manifest.size())});
  outputs.push_back({
      "container-rescue-lossless-v1.hex",
      make_lossless_rescue_container_golden(rescue_manifest)});
  return outputs;
}

std::string compact_hex_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open existing golden fixture");
  }
  std::string compact;
  char value{};
  while (input.get(value)) {
    if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
      continue;
    }
    compact.push_back(value);
  }
  if (!input.eof()) {
    throw std::runtime_error("failed to read existing golden fixture");
  }
  return compact;
}

void write_new_golden_fixture(
    const std::filesystem::path& directory,
    const GoldenOutput& output) {
  const std::filesystem::path path = directory / output.name;
  const std::string encoded = to_hex(output.bytes);
  if (std::filesystem::exists(path)) {
    if (compact_hex_file(path) != encoded) {
      throw std::runtime_error(
          "refusing to overwrite a different sealed golden fixture: " +
          path.string());
    }
  } else {
    std::ofstream file(path, std::ios::binary | std::ios::out);
    if (!file.is_open()) {
      throw std::runtime_error(
          "failed to create new golden fixture: " + path.string());
    }
    constexpr std::size_t kLineHexCharacters = 128U;
    for (std::size_t offset = 0U; offset < encoded.size();
         offset += kLineHexCharacters) {
      file.write(
          encoded.data() + offset,
          static_cast<std::streamsize>((std::min)(
              kLineHexCharacters, encoded.size() - offset)));
      file.put('\n');
    }
    if (!file.good()) {
      throw std::runtime_error(
          "failed to finish new golden fixture: " + path.string());
    }
  }
  const auto digest = ytec::imageformat::sha256(output.bytes);
  if (!digest.has_value()) {
    throw std::runtime_error("failed to hash emitted golden fixture");
  }
  std::cout << to_hex(digest.value()) << "  "
            << output.name.substr(0U, output.name.size() - 4U) << "  "
            << output.bytes.size() << '\n';
}

void audit_emit_golden_corpus(const std::filesystem::path& directory) {
  // Audit-only authoring boundary. Normal CTest reaches neither this function
  // nor filesystem I/O; the explicit command and permanent-fixture warning are
  // documented in tests/ImageGolden/README.md.
  if (!std::filesystem::is_directory(directory)) {
    throw std::runtime_error(
        "golden audit output must be an existing directory");
  }
  for (const auto& output : make_golden_outputs()) {
    write_new_golden_fixture(directory, output);
  }
}

std::vector<std::vector<std::byte>> make_seed_corpus() {
  std::vector<std::vector<std::byte>> corpus;
  corpus.reserve(kBoundarySeedCount + kGoldenFixtureCount);
  corpus.emplace_back();
  corpus.emplace_back(1U, std::byte{0});
  corpus.emplace_back(511U, std::byte{0});
  corpus.emplace_back(512U, std::byte{0xFF});
  corpus.emplace_back(kMaximumInputBytes, std::byte{0});
  auto golden = make_golden_outputs();
  if (golden.size() != kGoldenFixtureCount) {
    throw std::runtime_error("golden seed count drifted from the sealed corpus");
  }
  for (auto& fixture : golden) {
    if (fixture.bytes.size() > kMaximumInputBytes) {
      throw std::runtime_error("golden seed exceeded the fixed fuzz input bound");
    }
    corpus.push_back(std::move(fixture.bytes));
  }
  return corpus;
}

void fill_random(
    std::vector<std::byte>& bytes,
    DeterministicRandom& random) noexcept {
  for (auto& value : bytes) {
    value = random.byte();
  }
}

std::vector<std::byte> mutate(
    const std::vector<std::vector<std::byte>>& corpus,
    DeterministicRandom& random) {
  std::vector<std::byte> input = corpus[random.index(corpus.size())];
  const std::size_t operation = random.index(7U);

  if (operation == 0U) {
    input.assign(random.index(kMaximumInputBytes + 1U), std::byte{0});
    fill_random(input, random);
  } else if (operation == 1U && !input.empty()) {
    const std::size_t changes = 1U + random.index(16U);
    for (std::size_t index = 0; index < changes; ++index) {
      input[random.index(input.size())] ^= random.byte();
    }
  } else if (operation == 2U && !input.empty()) {
    input.resize(random.index(input.size()));
  } else if (operation == 3U && input.size() < kMaximumInputBytes) {
    const std::size_t available = kMaximumInputBytes - input.size();
    const std::size_t appended = 1U + random.index(
        (std::min)(available, static_cast<std::size_t>(256U)));
    const std::size_t old_size = input.size();
    input.resize(old_size + appended);
    for (std::size_t index = old_size; index < input.size(); ++index) {
      input[index] = random.byte();
    }
  } else if (operation == 4U && !input.empty()) {
    const std::size_t first = random.index(input.size());
    const std::size_t available = input.size() - first;
    const std::size_t count = 1U + random.index(available);
    input.erase(
        input.begin() + static_cast<std::ptrdiff_t>(first),
        input.begin() + static_cast<std::ptrdiff_t>(first + count));
  } else if (operation == 5U && !input.empty()) {
    const std::size_t first = random.index(input.size());
    const std::size_t count = (std::min)(
        input.size() - first,
        1U + random.index(32U));
    const std::byte replacement =
        random.index(2U) == 0U ? std::byte{0} : std::byte{0xFF};
    std::fill_n(
        input.begin() + static_cast<std::ptrdiff_t>(first),
        count,
        replacement);
  } else if (operation == 6U && input.size() < kMaximumInputBytes) {
    const std::size_t insert_at = random.index(input.size() + 1U);
    const std::size_t count = 1U + random.index(
        (std::min)(
            kMaximumInputBytes - input.size(),
            static_cast<std::size_t>(64U)));
    input.insert(
        input.begin() + static_cast<std::ptrdiff_t>(insert_at),
        count,
        random.byte());
  }

  if (input.size() > kMaximumInputBytes) {
    throw std::runtime_error("mutator exceeded the fixed input bound");
  }
  return input;
}

void exercise_input(
    const std::vector<std::byte>& input,
    FuzzStatistics& statistics,
    const bool allow_golden_password) {
  using namespace ytec::imageformat;
  ++statistics.inputs;

  auto tsumugi = inspect_tsumugi_v1(input);
  if (!tsumugi.has_value() && allow_golden_password) {
    tsumugi = inspect_tsumugi_v1(input, {.password = kGoldenPassword});
  }
  if (tsumugi.has_value()) {
    ++statistics.accepted_tsumugi;
    const auto& inspected = tsumugi.value();
    if (!inspected.header_hash_verified ||
        !inspected.all_chunks_verified ||
        !inspected.global_hash_verified) {
      throw std::runtime_error(
          "accepted .tsumugi lacks a required verification result");
    }
    switch (inspected.header.payload_kind) {
      case TsumugiPayloadKind::exact_disk:
        ++statistics.accepted_exact_tsumugi;
        break;
      case TsumugiPayloadKind::shrink_disk:
        ++statistics.accepted_shrink_tsumugi;
        break;
      case TsumugiPayloadKind::rescue_disk:
        ++statistics.accepted_rescue_tsumugi;
        break;
      default:
        throw std::runtime_error(
            "accepted .tsumugi has an unknown payload kind");
    }
    if ((inspected.header.required_features &
         static_cast<std::uint32_t>(TsumugiRequiredFeature::encrypted)) !=
        0U) {
      ++statistics.accepted_encrypted_tsumugi;
    }
  }

  const auto manifest = inspect_tsumugi_manifest_v1(input);
  if (manifest.has_value()) {
    ++statistics.accepted_manifests;
    const auto rebuilt = build_tsumugi_manifest_v1(manifest.value());
    if (!rebuilt.has_value() || rebuilt.value() != input) {
      throw std::runtime_error(
          "accepted Tsumugi manifest is not canonical");
    }
    switch (manifest.value().mode) {
      case TsumugiManifestMode::exact:
        ++statistics.accepted_exact_manifests;
        break;
      case TsumugiManifestMode::shrink:
        ++statistics.accepted_shrink_manifests;
        break;
      case TsumugiManifestMode::rescue:
        ++statistics.accepted_rescue_manifests;
        break;
      default:
        throw std::runtime_error(
            "accepted Tsumugi manifest has an unknown mode");
    }
    if (manifest.value().partition_style ==
        TsumugiManifestPartitionStyle::gpt) {
      ++statistics.accepted_gpt_manifests;
    }
  }

  const auto snapshot = inspect_partition_snapshot_v1(input);
  if (snapshot.has_value()) {
    ++statistics.accepted_snapshots;
    const auto rebuilt = build_partition_snapshot_v1(snapshot.value());
    if (!rebuilt.has_value() || rebuilt.value() != input) {
      throw std::runtime_error(
          "accepted partition snapshot is not canonical");
    }
    switch (snapshot.value().style) {
      case PartitionTableStyle::mbr:
        ++statistics.accepted_mbr_snapshots;
        break;
      case PartitionTableStyle::gpt:
        ++statistics.accepted_gpt_snapshots;
        break;
      default:
        throw std::runtime_error(
            "accepted partition snapshot has an unknown style");
    }
  }
}

void verify_golden_seed_coverage(const FuzzStatistics& statistics) {
  if (statistics.inputs != kBoundarySeedCount + kGoldenFixtureCount ||
      statistics.accepted_tsumugi != 9U ||
      statistics.accepted_exact_tsumugi != 5U ||
      statistics.accepted_shrink_tsumugi != 1U ||
      statistics.accepted_rescue_tsumugi != 3U ||
      statistics.accepted_encrypted_tsumugi != 1U ||
      statistics.accepted_manifests != 5U ||
      statistics.accepted_exact_manifests != 3U ||
      statistics.accepted_shrink_manifests != 1U ||
      statistics.accepted_rescue_manifests != 1U ||
      statistics.accepted_gpt_manifests != 1U ||
      statistics.accepted_snapshots != 3U ||
      statistics.accepted_mbr_snapshots != 2U ||
      statistics.accepted_gpt_snapshots != 1U) {
    throw std::runtime_error(
        "the in-memory Golden corpus no longer reaches every sealed format class");
  }
}

std::uint64_t parse_unsigned(
    std::string_view value,
    const char* const field) {
  int base = 10;
  if (value.starts_with("0x") || value.starts_with("0X")) {
    value.remove_prefix(2U);
    base = 16;
  }
  std::uint64_t parsed{};
  const auto result = std::from_chars(
      value.data(), value.data() + value.size(), parsed, base);
  if (value.empty() || result.ec != std::errc{} ||
      result.ptr != value.data() + value.size()) {
    throw std::runtime_error(
        std::string("invalid deterministic soak ") + field);
  }
  return parsed;
}

}  // namespace

int main(const int argc, const char* const* const argv) {
  std::size_t current_case = 0U;
  std::uint64_t campaign_seed = kRandomSeed;
  std::size_t mutation_iterations = kMutationIterations;
  std::string_view campaign_mode = "deterministic-smoke";
  try {
    if (argc == 4 &&
        std::string_view(argv[1]) == "--audit-emit-golden-corpus" &&
        std::string_view(argv[3]) == kGoldenEmitConfirmation) {
      audit_emit_golden_corpus(std::filesystem::path(argv[2]));
      return 0;
    }
    if (argc == 4 &&
        std::string_view(argv[1]) == "--deterministic-soak") {
      campaign_seed = parse_unsigned(argv[2], "seed");
      const auto parsed_iterations = parse_unsigned(argv[3], "iterations");
      if (campaign_seed == 0U || parsed_iterations == 0U ||
          parsed_iterations > kMaximumSoakMutations ||
          parsed_iterations > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            "deterministic soak seed or iteration bound is invalid");
      }
      mutation_iterations = static_cast<std::size_t>(parsed_iterations);
      campaign_mode = "deterministic-soak";
    } else if (argc != 1) {
      throw std::runtime_error(
          "unsupported arguments; use the documented Golden audit or "
          "deterministic soak command");
    }
    auto corpus = make_seed_corpus();
    FuzzStatistics statistics;
    for (const auto& seed : corpus) {
      exercise_input(seed, statistics, true);
      ++current_case;
    }
    verify_golden_seed_coverage(statistics);

    DeterministicRandom random(campaign_seed);
    for (std::size_t iteration = 0U;
         iteration < mutation_iterations;
         ++iteration) {
      const auto input = mutate(corpus, random);
      exercise_input(input, statistics, false);
      ++current_case;
    }

    std::cout
        << "PASS ytec-image-fuzz-tests mode=" << campaign_mode
        << " coverage_guided=false seed=0x" << std::hex << campaign_seed
        << std::dec
        << " cases=" << statistics.inputs
        << " max_input_bytes=" << kMaximumInputBytes
        << " golden_seeds=" << kGoldenFixtureCount
        << " accepted_tsumugi=" << statistics.accepted_tsumugi
        << " exact=" << statistics.accepted_exact_tsumugi
        << " shrink=" << statistics.accepted_shrink_tsumugi
        << " rescue=" << statistics.accepted_rescue_tsumugi
        << " encrypted=" << statistics.accepted_encrypted_tsumugi
        << " accepted_manifests=" << statistics.accepted_manifests
        << " manifest_exact=" << statistics.accepted_exact_manifests
        << " manifest_shrink=" << statistics.accepted_shrink_manifests
        << " manifest_rescue=" << statistics.accepted_rescue_manifests
        << " manifest_gpt=" << statistics.accepted_gpt_manifests
        << " accepted_snapshots=" << statistics.accepted_snapshots
        << " snapshot_mbr=" << statistics.accepted_mbr_snapshots
        << " snapshot_gpt=" << statistics.accepted_gpt_snapshots
        << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr
        << "FAIL ytec-image-fuzz-tests mode=" << campaign_mode
        << " coverage_guided=false seed=0x" << std::hex << campaign_seed
        << std::dec
        << " case=" << current_case
        << ": " << exception.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr
        << "FAIL ytec-image-fuzz-tests mode=" << campaign_mode
        << " coverage_guided=false seed=0x" << std::hex << campaign_seed
        << std::dec
        << " case=" << current_case
        << ": unknown exception\n";
    return 1;
  }
}

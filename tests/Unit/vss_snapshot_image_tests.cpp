#include "ytec/vssrequester/snapshot_image.h"
#include "ytec/vssrequester/snapshot_copy.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

ytec::clonecore::Error test_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_READ_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

class PatternSnapshotReader final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit PatternSnapshotReader(
      const std::uint64_t size,
      const std::uint64_t ntfs_size = 0)
      : size_(size), ntfs_size_(ntfs_size == 0 ? size : ntfs_size) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_count;
    if (fail_reads) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モックVSS Snapshot読取り", L"注入失敗です"));
    }
    if (offset > size_ || length > size_ - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モックVSS Snapshot範囲", L"範囲外です"));
    }
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes[index] =
          static_cast<std::byte>((offset + index) & 0xFFU);
    }
    if (offset == 0 && length >= 512) {
      constexpr char kNtfsSignature[] = "NTFS    ";
      std::memcpy(bytes.data() + 3, kNtfsSignature, 8);
      write_little<std::uint16_t>(bytes, 11, 512);
      bytes[13] = std::byte{8};
      write_little<std::uint64_t>(bytes, 40, ntfs_size_ / 512);
      bytes[510] = std::byte{0x55};
      bytes[511] = std::byte{0xAA};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  mutable std::size_t read_count{};
  bool fail_reads{};

 private:
  std::uint64_t size_{};
  std::uint64_t ntfs_size_{};
};

class MockBitmapProvider final
    : public ytec::clonecore::INtfsUsedRangeProvider {
 public:
  [[nodiscard]]
  ytec::clonecore::Result<std::vector<ytec::clonecore::ByteRange>>
  query_used_ranges(
      const std::uint32_t partition_index,
      const ytec::clonecore::NtfsGeometry& geometry) override {
    ++query_count;
    observed_partition = partition_index;
    observed_cluster_size = geometry.cluster_size();
    if (fail_query) {
      return ytec::clonecore::Result<
          std::vector<ytec::clonecore::ByteRange>>::failure(
          test_error(L"モックSnapshot Bitmap", L"注入失敗です"));
    }
    return ytec::clonecore::Result<
        std::vector<ytec::clonecore::ByteRange>>::success(ranges);
  }

  std::vector<ytec::clonecore::ByteRange> ranges;
  std::size_t query_count{};
  std::uint32_t observed_partition{};
  std::uint64_t observed_cluster_size{};
  bool fail_query{};
};

class MemoryStagingTarget final
    : public ytec::imageformat::IDcimgStagingTarget {
 public:
  [[nodiscard]] ytec::clonecore::Status begin(
      const std::uint64_t expected_length) override {
    ++begin_count;
    if (expected_length > 64ULL * 1024ULL * 1024ULL) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックVSS出力開始", L"試験上限超過です"));
    }
    bytes.assign(
        static_cast<std::size_t>(expected_length), std::byte{0});
    begun = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> data) override {
    if (!begun || offset > bytes.size() ||
        data.size() > bytes.size() - offset) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックVSS出力書込み", L"範囲外です"));
    }
    std::copy(
        data.begin(),
        data.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]]
  ytec::clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!begun || offset > bytes.size() ||
        length > bytes.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モックVSS出力読戻し", L"範囲外です"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  }

  [[nodiscard]] ytec::clonecore::Status flush() override {
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    if (!begun || final_length == 0U || final_length > bytes.size()) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックVSS出力最終長", L"範囲外です"));
    }
    bytes.resize(static_cast<std::size_t>(final_length));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status commit_verified() override {
    committed = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status abort_incomplete() override {
    aborted = true;
    begun = false;
    bytes.clear();
    return ytec::clonecore::success_status();
  }

  std::vector<std::byte> bytes;
  std::size_t begin_count{};
  bool begun{};
  bool committed{};
  bool aborted{};
};

ytec::vssrequester::VssSnapshotImageRequest valid_request(
    const PatternSnapshotReader& reader) {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  ytec::vssrequester::VssSnapshotImageRequest request;
  request.source_disk_size = 64ULL * kMiB;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.verification_block_bytes = 1024U * 1024U;
  request.manifest = {
      std::byte{'V'}, std::byte{'S'}, std::byte{'S'}, std::byte{'1'}};
  request.partition_table_snapshot.assign(512, std::byte{0});
  request.volumes.push_back(
      ytec::vssrequester::VssSnapshotImageVolume{
          .partition_entry_index = 2,
          .disk_offset = 16ULL * kMiB,
          .partition_length = 32ULL * kMiB,
          .geometry =
              ytec::clonecore::NtfsGeometry{
                  .bytes_per_sector = 512,
                  .sectors_per_cluster = 8,
                  .total_sectors = (32ULL * kMiB) / 512ULL,
              },
          .snapshot_reader = &reader,
      });
  return request;
}

ytec::vssrequester::SnapshotImageCopyRequest valid_copy_request() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  return ytec::vssrequester::SnapshotImageCopyRequest{
      .source_disk_size = 64ULL * kMiB,
      .logical_sector_size = 512,
      .physical_sector_size = 4096,
      .chunk_size = ytec::imageformat::kDcimgChunkSize16MiB,
      .verification_block_bytes = 1024U * 1024U,
      .manifest = {
          std::byte{'V'}, std::byte{'S'}, std::byte{'S'}, std::byte{'2'}},
      .partition_table_snapshot =
          std::vector<std::byte>(512, std::byte{0}),
      .volumes = {
          ytec::vssrequester::SnapshotImageVolumePlan{
              .partition_entry_index = 2,
              .disk_offset = 16ULL * kMiB,
              .partition_length = 32ULL * kMiB,
          },
      },
  };
}

void test_bitmap_ranges_stream_snapshot_and_split_chunks() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB);
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
      ytec::clonecore::ByteRange{
          .offset = 8192,
          .length = 16ULL * kMiB + 4096,
      },
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      valid_request(reader), bitmap, target);
  check(result.has_value(), "Valid Snapshot ranges should build a dcimg");
  check(target.committed && !target.aborted,
        "Verified VSS image should commit");
  check(bitmap.query_count == 1 && bitmap.observed_partition == 2 &&
            bitmap.observed_cluster_size == 4096,
        "The bound partition and NTFS geometry must reach the provider");
  check(result.value().chunk_count == 3,
        "A range larger than 16 MiB must split into independent chunks");
  check(result.value().stored_data_bytes == 16ULL * kMiB + 8192,
        "Only used Snapshot bytes should be stored");

  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(target.bytes);
  check(inspection.has_value(),
        "The VSS stream must pass the existing untrusted parser");
  const auto& chunks = inspection.value().chunks;
  check(
      chunks.size() == 3 &&
          chunks[0].logical_offset == 16ULL * kMiB &&
          chunks[1].logical_offset == 16ULL * kMiB + 8192 &&
          chunks[2].logical_offset ==
              32ULL * kMiB + 8192,
      "Snapshot-local offsets must map to canonical disk offsets");
}

void test_zstandard_profile_reaches_verified_vss_container() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB);
  auto request = valid_request(reader);
  request.compression =
      ytec::imageformat::DcimgCompression::zstandard;
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(
      result.has_value() && target.committed &&
          result.value().stored_data_bytes < 4096,
      "VSS should commit a smaller verified Zstandard payload");
  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(target.bytes);
  check(
      inspection.has_value() &&
          inspection.value().header.compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          inspection.value().header.compression_version ==
              ytec::imageformat::kDcimgZstandardProfileVersion &&
          inspection.value().chunks.front().compression ==
              ytec::imageformat::DcimgCompression::zstandard,
      "VSS container and chunk records must declare profile 1 canonically");
}

void test_bitmap_out_of_range_stops_before_staging() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB);
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{
          .offset = 32ULL * kMiB,
          .length = 4096,
      },
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      valid_request(reader), bitmap, target);
  check(!result.has_value(), "An out-of-range bitmap must fail");
  check(target.begin_count == 0 && !target.committed,
        "Invalid bitmap data must not create staged output");
}

void test_reader_geometry_mismatch_stops_before_bitmap() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(31ULL * kMiB);
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      valid_request(reader), bitmap, target);
  check(!result.has_value(), "Snapshot size mismatch must fail");
  check(bitmap.query_count == 0 && target.begin_count == 0,
        "Identity mismatch must stop before bitmap and output");
}

void test_ntfs_smaller_than_partition_is_preserved_as_sparse_tail() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB, 31ULL * kMiB);
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
      ytec::clonecore::ByteRange{
          .offset = 31ULL * kMiB - 4096,
          .length = 4096,
      },
  };
  MemoryStagingTarget target;
  auto request = valid_request(reader);
  request.volumes.front().geometry.total_sectors =
      (31ULL * kMiB) / 512ULL;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(
      result.has_value() && target.committed,
      "An NTFS volume contained by a larger partition should be accepted");
}

void test_bitmap_range_in_partition_tail_is_rejected() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB, 31ULL * kMiB);
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
      ytec::clonecore::ByteRange{
          .offset = 31ULL * kMiB,
          .length = 4096,
      },
  };
  MemoryStagingTarget target;
  auto request = valid_request(reader);
  request.volumes.front().geometry.total_sectors =
      (31ULL * kMiB) / 512ULL;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(!result.has_value(), "A bitmap range past the NTFS end must fail");
  check(
      bitmap.query_count == 1 && target.begin_count == 0,
      "An NTFS-tail range must stop before staged output");
}

void test_bitmap_query_failure_stops_before_staging() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB);
  MockBitmapProvider bitmap;
  bitmap.fail_query = true;
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      valid_request(reader), bitmap, target);
  check(!result.has_value(), "Bitmap query failure must stop");
  check(bitmap.query_count == 1 && target.begin_count == 0,
        "Bitmap failure must not create staged output");
}

void test_snapshot_disappearance_aborts_incomplete_output() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader reader(32ULL * kMiB);
  reader.fail_reads = true;
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      valid_request(reader), bitmap, target);
  check(!result.has_value(), "Snapshot read failure must stop");
  check(target.aborted && !target.committed,
        "Snapshot disappearance must discard the incomplete image");
}

void test_duplicate_partition_binding_stops_before_bitmap() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader first(32ULL * kMiB);
  PatternSnapshotReader second(8ULL * kMiB);
  auto request = valid_request(first);
  request.volumes.push_back(
      ytec::vssrequester::VssSnapshotImageVolume{
          .partition_entry_index = 2,
          .disk_offset = 48ULL * kMiB,
          .partition_length = 8ULL * kMiB,
          .geometry =
              ytec::clonecore::NtfsGeometry{
                  .bytes_per_sector = 512,
                  .sectors_per_cluster = 8,
                  .total_sectors = (8ULL * kMiB) / 512ULL,
              },
          .snapshot_reader = &second,
      });
  MockBitmapProvider bitmap;
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(!result.has_value(), "Duplicate partition binding must fail");
  check(bitmap.query_count == 0 && target.begin_count == 0,
        "Duplicate identity must stop before bitmap and output");
}

void test_raw_efi_region_and_vss_ntfs_share_verified_image() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader snapshot_reader(32ULL * kMiB);
  PatternSnapshotReader physical_reader(64ULL * kMiB);
  auto request = valid_request(snapshot_reader);
  request.raw_regions.push_back(
      ytec::vssrequester::VssSnapshotImageRawRegion{
          .disk_offset = 1ULL * kMiB,
          .length = 2ULL * kMiB,
          .source_offset = 1ULL * kMiB,
          .source_reader = &physical_reader,
      });
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(
      result.has_value() && target.committed,
      "Raw EFI and VSS NTFS data should commit one verified image");
  check(
      result.value().stored_data_bytes == 2ULL * kMiB + 4096,
      "Image should contain the complete raw range and used NTFS bytes");
  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(target.bytes);
  check(inspection.has_value(), "Mixed-source image should inspect");
  check(
      inspection.value().chunks.size() == 2 &&
          inspection.value().chunks[0].logical_offset == 1ULL * kMiB &&
          inspection.value().chunks[1].logical_offset == 16ULL * kMiB,
      "Mixed source chunks must be sorted by disk logical offset");
}

void test_raw_region_overlap_stops_before_bitmap_and_output() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader snapshot_reader(32ULL * kMiB);
  PatternSnapshotReader physical_reader(64ULL * kMiB);
  auto request = valid_request(snapshot_reader);
  request.raw_regions.push_back(
      ytec::vssrequester::VssSnapshotImageRawRegion{
          .disk_offset = 16ULL * kMiB,
          .length = 1ULL * kMiB,
          .source_offset = 0,
          .source_reader = &physical_reader,
      });
  MockBitmapProvider bitmap;
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(!result.has_value(), "Raw and Snapshot overlap must fail");
  check(
      bitmap.query_count == 0 && target.begin_count == 0,
      "Mixed-source overlap must stop before bitmap and output");
}

void test_raw_reader_failure_aborts_incomplete_output() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  PatternSnapshotReader snapshot_reader(32ULL * kMiB);
  PatternSnapshotReader physical_reader(64ULL * kMiB);
  physical_reader.fail_reads = true;
  auto request = valid_request(snapshot_reader);
  request.raw_regions.push_back(
      ytec::vssrequester::VssSnapshotImageRawRegion{
          .disk_offset = 1ULL * kMiB,
          .length = 1ULL * kMiB,
          .source_offset = 1ULL * kMiB,
          .source_reader = &physical_reader,
      });
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result = ytec::vssrequester::write_vss_snapshot_dcimg_v1(
      request, bitmap, target);
  check(!result.has_value(), "Raw source disappearance must fail");
  check(
      target.aborted && !target.committed,
      "Raw source failure must discard incomplete output");
}

void test_snapshot_paths_open_and_build_verified_image() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  const std::vector<std::wstring> paths{
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy42"};
  std::size_t open_count = 0;
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result =
      ytec::vssrequester::copy_snapshot_devices_to_dcimg_v1(
          valid_copy_request(),
          paths,
          [&](const ytec::vssrequester::SnapshotVolumeOpenRequest& request)
              -> ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>> {
            ++open_count;
            check(
                request.snapshot_device_path == paths.front() &&
                    request.expected_size_bytes == 32ULL * kMiB &&
                    request.logical_sector_size == 512,
                "Snapshot opener must receive the bound path and geometry");
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::success(
                std::make_unique<PatternSnapshotReader>(32ULL * kMiB));
          },
          bitmap,
          target);
  check(result.has_value(), "Snapshot connector should build a valid image");
  check(
      open_count == 1 && bitmap.query_count == 1 &&
          bitmap.observed_partition == 2 && target.committed,
      "Reader, bound bitmap, and verified commit must run once");
  check(
      ytec::imageformat::inspect_dcimg_v1(target.bytes).has_value(),
      "Connector output must pass the untrusted dcimg parser");
}

void test_connector_accepts_16k_physical_sector() {
  const std::vector<std::wstring> paths{
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy45"};
  auto request = valid_copy_request();
  request.physical_sector_size = 16U * 1024U;
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result =
      ytec::vssrequester::copy_snapshot_devices_to_dcimg_v1(
          request,
          paths,
          [](const ytec::vssrequester::SnapshotVolumeOpenRequest& value)
              -> ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>> {
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::success(
                std::make_unique<PatternSnapshotReader>(
                    value.expected_size_bytes));
          },
          bitmap,
          target);
  check(
      result.has_value() && target.committed,
      "A validated 16 KiB physical sector source should reach dcimg commit");
  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(target.bytes);
  check(
      inspection.has_value() &&
          inspection.value().header.physical_sector_size ==
              16U * 1024U,
      "The dcimg header should preserve the 16 KiB physical sector size");
}

void test_connector_accepts_ntfs_smaller_than_partition() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  const std::vector<std::wstring> paths{
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy44"};
  MockBitmapProvider bitmap;
  bitmap.ranges = {
      ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
  };
  MemoryStagingTarget target;
  const auto result =
      ytec::vssrequester::copy_snapshot_devices_to_dcimg_v1(
          valid_copy_request(),
          paths,
          [](const ytec::vssrequester::SnapshotVolumeOpenRequest& request)
              -> ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>> {
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::success(
                std::make_unique<PatternSnapshotReader>(
                    request.expected_size_bytes,
                    31ULL * kMiB));
          },
          bitmap,
          target);
  check(
      result.has_value() && target.committed,
      "The connector should accept a contained NTFS volume");
}

void test_live_volume_path_is_rejected_before_open() {
  const std::vector<std::wstring> paths{
      L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\"};
  std::size_t open_count = 0;
  MockBitmapProvider bitmap;
  MemoryStagingTarget target;
  const auto result =
      ytec::vssrequester::copy_snapshot_devices_to_dcimg_v1(
          valid_copy_request(),
          paths,
          [&](const ytec::vssrequester::SnapshotVolumeOpenRequest&)
              -> ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>> {
            ++open_count;
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::failure(
                test_error(L"呼ばれてはいけないopener", L"境界違反です"));
          },
          bitmap,
          target);
  check(!result.has_value(), "Live Volume GUID path must fail closed");
  check(
      open_count == 0 && bitmap.query_count == 0 &&
          target.begin_count == 0,
      "Invalid path must stop before reader, bitmap, and output");
}

void test_connector_reader_identity_mismatch_stops_before_bitmap() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  const std::vector<std::wstring> paths{
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy43"};
  MockBitmapProvider bitmap;
  MemoryStagingTarget target;
  const auto result =
      ytec::vssrequester::copy_snapshot_devices_to_dcimg_v1(
          valid_copy_request(),
          paths,
          [&](const ytec::vssrequester::SnapshotVolumeOpenRequest&)
              -> ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>> {
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::success(
                std::make_unique<PatternSnapshotReader>(31ULL * kMiB));
          },
          bitmap,
          target);
  check(!result.has_value(), "Reader size substitution must fail");
  check(
      bitmap.query_count == 0 && target.begin_count == 0,
      "Reader identity mismatch must stop before bitmap and output");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"bitmap_ranges_stream_snapshot_and_split_chunks",
       test_bitmap_ranges_stream_snapshot_and_split_chunks},
      {"zstandard_profile_reaches_verified_vss_container",
       test_zstandard_profile_reaches_verified_vss_container},
      {"bitmap_out_of_range_stops_before_staging",
       test_bitmap_out_of_range_stops_before_staging},
      {"reader_geometry_mismatch_stops_before_bitmap",
       test_reader_geometry_mismatch_stops_before_bitmap},
      {"ntfs_smaller_than_partition_is_preserved_as_sparse_tail",
       test_ntfs_smaller_than_partition_is_preserved_as_sparse_tail},
      {"bitmap_range_in_partition_tail_is_rejected",
       test_bitmap_range_in_partition_tail_is_rejected},
      {"bitmap_query_failure_stops_before_staging",
       test_bitmap_query_failure_stops_before_staging},
      {"snapshot_disappearance_aborts_incomplete_output",
       test_snapshot_disappearance_aborts_incomplete_output},
      {"duplicate_partition_binding_stops_before_bitmap",
       test_duplicate_partition_binding_stops_before_bitmap},
      {"raw_efi_region_and_vss_ntfs_share_verified_image",
       test_raw_efi_region_and_vss_ntfs_share_verified_image},
      {"raw_region_overlap_stops_before_bitmap_and_output",
       test_raw_region_overlap_stops_before_bitmap_and_output},
      {"raw_reader_failure_aborts_incomplete_output",
       test_raw_reader_failure_aborts_incomplete_output},
      {"snapshot_paths_open_and_build_verified_image",
       test_snapshot_paths_open_and_build_verified_image},
      {"connector_accepts_16k_physical_sector",
       test_connector_accepts_16k_physical_sector},
      {"connector_accepts_ntfs_smaller_than_partition",
       test_connector_accepts_ntfs_smaller_than_partition},
      {"live_volume_path_is_rejected_before_open",
       test_live_volume_path_is_rejected_before_open},
      {"connector_reader_identity_mismatch_stops_before_bitmap",
       test_connector_reader_identity_mismatch_stops_before_bitmap},
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
  return failures == 0 ? 0 : 1;
}

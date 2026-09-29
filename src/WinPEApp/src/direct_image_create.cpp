#include "ytec/winpeapp/direct_image_create.h"

#include "ytec/winpeapp/direct_image_create_resume.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_create_resume.h"
#include "ytec/imageformat/tsumugi_crypto.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/operationcore/checkpoint.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::uint32_t kDirectImageChunkBytes =
    imageformat::kImageChunkSize16MiB;

clonecore::Error image_error(
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
  return clonecore::Result<T>::failure(image_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_domain(
    std::vector<std::byte>& bytes,
    const std::string_view domain) {
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  bytes.push_back(std::byte{0});
}

clonecore::Status append_ascii(
    std::vector<std::byte>& bytes,
    const std::string_view value,
    const std::wstring_view operation) {
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が識別Hashの上限を超えています"));
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
  return clonecore::success_status();
}

clonecore::Status append_utf16(
    std::vector<std::byte>& bytes,
    const std::wstring_view value,
    const std::wstring_view operation) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"文字列が識別Hashの上限を超えています"));
  }
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t code_unit : value) {
    const auto value16 = static_cast<std::uint16_t>(code_unit);
    bytes.push_back(static_cast<std::byte>(value16 & 0xffU));
    bytes.push_back(static_cast<std::byte>((value16 >> 8U) & 0xffU));
  }
  return clonecore::success_status();
}

bool ends_with_tsumugi(const std::wstring_view path) noexcept {
  constexpr std::wstring_view extension = L".tsumugi";
  return path.size() > extension.size() &&
      _wcsnicmp(
          path.data() + path.size() - extension.size(),
          extension.data(),
          extension.size()) == 0;
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      left.type == right.type && left.identifier == right.identifier &&
      left.name == right.name && left.bootable == right.bootable;
}

bool same_reviewed_layout(
    const diskmodel::DiskInfo& reviewed,
    const diskmodel::DiskInfo& observed) {
  if (reviewed.physical_sector_size != observed.physical_sector_size ||
      reviewed.partition_style != observed.partition_style ||
      reviewed.partitions.size() != observed.partitions.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < reviewed.partitions.size(); ++index) {
    if (!same_partition(reviewed.partitions[index], observed.partitions[index])) {
      return false;
    }
  }
  return true;
}

bool same_partition_geometry(
    std::vector<diskmodel::PartitionInfo> inventory,
    std::vector<std::pair<std::uint64_t, std::uint64_t>> parsed) {
  if (inventory.size() != parsed.size()) {
    return false;
  }
  std::sort(
      inventory.begin(), inventory.end(), [](const auto& left, const auto& right) {
        return left.offset_bytes < right.offset_bytes;
      });
  std::sort(parsed.begin(), parsed.end());
  for (std::size_t index = 0U; index < inventory.size(); ++index) {
    if (inventory[index].offset_bytes != parsed[index].first ||
        inventory[index].size_bytes != parsed[index].second) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_gpt_inventory_geometry(
    const diskmodel::DiskInfo& inventory,
    const clonecore::GptDisk& layout) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> parsed;
  parsed.reserve(layout.partitions.size());
  for (const auto& partition : layout.partitions) {
    std::uint64_t offset{};
    std::uint64_t sectors{};
    std::uint64_t length{};
    if (partition.last_lba < partition.first_lba ||
        !checked_multiply(
            partition.first_lba, layout.logical_sector_size, offset) ||
        !checked_add(
            partition.last_lba - partition.first_lba, 1U, sectors) ||
        !checked_multiply(sectors, layout.logical_sector_size, length)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPTレビュー照合",
          L"RAW解析したGPT範囲が不正です"));
    }
    parsed.emplace_back(offset, length);
  }
  if (!same_partition_geometry(inventory.partitions, std::move(parsed))) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Tsumugi GPTレビュー照合",
        L"レビュー済み一覧とRAW解析したGPTパーティション範囲が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_mbr_inventory_geometry(
    const diskmodel::DiskInfo& inventory,
    const clonecore::MbrDisk& layout) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> parsed;
  parsed.reserve(layout.partitions.size());
  for (const auto& partition : layout.partitions) {
    std::uint64_t offset{};
    std::uint64_t length{};
    if (!checked_multiply(
            partition.first_lba, layout.logical_sector_size, offset) ||
        !checked_multiply(
            partition.sector_count, layout.logical_sector_size, length)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi MBRレビュー照合",
          L"RAW解析したMBR範囲が不正です"));
    }
    parsed.emplace_back(offset, length);
  }
  if (!same_partition_geometry(inventory.partitions, std::move(parsed))) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Tsumugi MBRレビュー照合",
        L"レビュー済み一覧とRAW解析したMBRパーティション範囲が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Result<const diskmodel::PartitionInfo*>
find_reviewed_partition_by_range(
    const diskmodel::DiskInfo& source,
    const std::uint64_t offset,
    const std::uint64_t length,
    const std::wstring_view operation) {
  const diskmodel::PartitionInfo* found = nullptr;
  for (const auto& partition : source.partitions) {
    if (partition.offset_bytes != offset || partition.size_bytes != length) {
      continue;
    }
    if (found != nullptr) {
      return failure<const diskmodel::PartitionInfo*>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          std::wstring(operation),
          L"同じ範囲へ複数のレビュー済みPartitionNumberが対応しています");
    }
    found = &partition;
  }
  if (found == nullptr) {
    return failure<const diskmodel::PartitionInfo*>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        std::wstring(operation),
        L"RAW解析範囲へ対応するレビュー済みPartitionNumberがありません");
  }
  return clonecore::Result<const diskmodel::PartitionInfo*>::success(found);
}

bool partition_required(
    const diskmodel::ImagePartitionSelection& selection,
    const std::uint32_t partition_number) noexcept {
  return std::find(
             selection.required_partition_numbers.begin(),
             selection.required_partition_numbers.end(),
             partition_number) != selection.required_partition_numbers.end();
}

std::vector<std::uint32_t> canonical_persisted_selection(
    const diskmodel::ImagePartitionSelection& selection) {
  return selection.whole_disk
      ? std::vector<std::uint32_t>{}
      : selection.selected_partition_numbers;
}

bool same_image_partition_selection(
    const diskmodel::ImagePartitionSelection& left,
    const diskmodel::ImagePartitionSelection& right) noexcept {
  return left.whole_disk == right.whole_disk &&
      left.contains_windows == right.contains_windows &&
      left.selected_bytes == right.selected_bytes &&
      left.selected_partition_numbers == right.selected_partition_numbers &&
      left.required_partition_numbers == right.required_partition_numbers;
}

struct SourceIdentityHashes final {
  imageformat::Sha256Digest model{};
  imageformat::Sha256Digest serial{};
  imageformat::Sha256Digest state{};
};

clonecore::Result<imageformat::Sha256Digest> hash_source_model(
    const std::wstring_view model) {
  return imageformat::hash_tsumugi_source_model_v1(model);
}

clonecore::Result<imageformat::Sha256Digest> hash_source_serial(
    const std::string_view serial_suffix,
    const std::wstring_view device_instance_id) {
  return imageformat::hash_tsumugi_source_serial_v1(
      serial_suffix, device_instance_id);
}

clonecore::Result<SourceIdentityHashes> make_source_hashes(
    const clonecore::StableDiskIdentity& source,
    const std::uint32_t physical_sector_size,
    const std::span<const std::byte> partition_snapshot) {
  const auto identity = clonecore::validate_stable_identity(
      source, source, L"PE Tsumugiコピー元");
  if (!identity ||
      !imageformat::is_supported_sector_size_pair(
          source.logical_sector_size, physical_sector_size) ||
      partition_snapshot.empty()) {
    return clonecore::Result<SourceIdentityHashes>::failure(
        identity
            ? image_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"PE Tsumugi Source状態Hash",
                  L"物理セクターまたはパーティションsnapshotがありません")
            : identity.error());
  }
  const auto snapshot = imageformat::inspect_partition_snapshot_v1(
      partition_snapshot);
  if (!snapshot || snapshot.value().source_disk_size != source.size_bytes ||
      snapshot.value().logical_sector_size != source.logical_sector_size) {
    return clonecore::Result<SourceIdentityHashes>::failure(
        snapshot
            ? image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"PE Tsumugi Source状態Hash",
                  L"安定識別とpartition snapshotの寸法が一致しません")
            : snapshot.error());
  }
  auto model = hash_source_model(source.model);
  if (!model) {
    return clonecore::Result<SourceIdentityHashes>::failure(model.error());
  }
  auto serial = hash_source_serial(
      source.serial_suffix, source.device_instance_id);
  if (!serial) {
    return clonecore::Result<SourceIdentityHashes>::failure(serial.error());
  }

  std::vector<std::byte> material;
  material.reserve(128U + partition_snapshot.size());
  append_domain(material, "YTEC-TSUMUGI-LOCKED-SOURCE-STATE-V1");
  const auto model_appended = append_utf16(
      material, source.model, L"PE Tsumugi Source状態Hash");
  const auto serial_appended = append_ascii(
      material, source.serial_suffix, L"PE Tsumugi Source状態Hash");
  const auto instance_appended = append_utf16(
      material, source.device_instance_id, L"PE Tsumugi Source状態Hash");
  if (!model_appended || !serial_appended || !instance_appended) {
    return clonecore::Result<SourceIdentityHashes>::failure(
        !model_appended
            ? model_appended.error()
            : !serial_appended ? serial_appended.error()
                               : instance_appended.error());
  }
  append_u64(material, source.size_bytes);
  append_u32(material, source.logical_sector_size);
  append_u32(material, physical_sector_size);
  material.push_back(source.is_system_disk ? std::byte{1} : std::byte{0});
  append_u64(
      material, static_cast<std::uint64_t>(partition_snapshot.size()));
  material.insert(
      material.end(), partition_snapshot.begin(), partition_snapshot.end());
  auto state = imageformat::sha256(material);
  if (!state) {
    return clonecore::Result<SourceIdentityHashes>::failure(state.error());
  }
  return clonecore::Result<SourceIdentityHashes>::success({
      .model = model.take_value(),
      .serial = serial.take_value(),
      .state = state.take_value(),
  });
}

class LockedSourceSession final
    : public imageformat::ITsumugiImageSourceSession {
 public:
  LockedSourceSession(
      std::unique_ptr<clonecore::ISourceDiskReader> reader,
      SourceIdentityHashes hashes) noexcept
      : reader_(std::move(reader)), hashes_(std::move(hashes)) {}

  std::uint64_t size_bytes() const noexcept override {
    return reader_ ? reader_->size_bytes() : 0U;
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return reader_ ? reader_->logical_sector_size() : 0U;
  }

  clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!reader_) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_HANDLE,
          L"PE Tsumugi Source読取り",
          L"読取り専用Sourceセッションがありません");
    }
    return reader_->read(offset, length);
  }

  imageformat::Sha256Digest source_model_hash() const noexcept override {
    return hashes_.model;
  }

  imageformat::Sha256Digest source_serial_hash() const noexcept override {
    return hashes_.serial;
  }

  imageformat::Sha256Digest source_state_hash() const noexcept override {
    return hashes_.state;
  }

 private:
  std::unique_ptr<clonecore::ISourceDiskReader> reader_;
  SourceIdentityHashes hashes_;
};

imageformat::TsumugiManifestPartitionRole gpt_role(
    const clonecore::GptGuid& type) noexcept {
  if (type == clonecore::gpt_type_efi_system()) {
    return imageformat::TsumugiManifestPartitionRole::efi_system;
  }
  if (type == clonecore::gpt_type_microsoft_reserved()) {
    return imageformat::TsumugiManifestPartitionRole::microsoft_reserved;
  }
  if (type == clonecore::gpt_type_windows_recovery()) {
    return imageformat::TsumugiManifestPartitionRole::recovery;
  }
  if (type == clonecore::gpt_type_basic_data()) {
    return imageformat::TsumugiManifestPartitionRole::data;
  }
  return imageformat::TsumugiManifestPartitionRole::other;
}

imageformat::TsumugiManifestFileSystem gpt_file_system(
    const clonecore::GptGuid& type) noexcept {
  if (type == clonecore::gpt_type_microsoft_reserved()) {
    return imageformat::TsumugiManifestFileSystem::none;
  }
  // A partition type does not prove the on-disk filesystem. Exact mode reads
  // the full raw range, so keep it unknown until a separate bounded
  // filesystem probe has positively identified it.
  return imageformat::TsumugiManifestFileSystem::unknown;
}

clonecore::Status append_partition_chunks(
    std::vector<imageformat::TsumugiStreamBuildChunk>& chunks,
    const std::uint64_t offset,
    const std::uint64_t length,
    const imageformat::ITsumugiImageSourceSession& source) {
  std::uint64_t position = 0U;
  while (position < length) {
    if (chunks.size() >= imageformat::kTsumugiMaximumChunkCount) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_TOO_MANY_OPEN_FILES,
          L"PE Tsumugiチャンク計画",
          L"画像チャンク数が形式上限を超えます"));
    }
    const auto amount = (std::min)(
        static_cast<std::uint64_t>(kDirectImageChunkBytes),
        length - position);
    std::uint64_t current{};
    if (!checked_add(offset, position, current)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugiチャンク範囲",
          L"コピー元範囲が64bit上限を超えます"));
    }
    chunks.push_back(imageformat::TsumugiStreamBuildChunk{
        .logical_offset = current,
        .logical_length = amount,
        .source_offset = current,
        .flags = imageformat::TsumugiChunkFlags::none,
        .source = &source,
    });
    position += amount;
  }
  return clonecore::success_status();
}

struct PreparedExactImage final {
  imageformat::TsumugiManifest manifest;
  std::vector<imageformat::TsumugiStreamBuildChunk> chunks;
  std::uint64_t logical_payload_bytes{};
  std::uint32_t selected_partition_count{};
};

clonecore::Result<PreparedExactImage> prepare_gpt_image(
    const DirectImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const clonecore::GptDisk& layout,
    const diskmodel::ImagePartitionSelection& selection,
    std::vector<std::byte> partition_snapshot,
    const SourceIdentityHashes& hashes,
    const imageformat::ITsumugiImageSourceSession& session) {
  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style = imageformat::TsumugiManifestPartitionStyle::gpt,
      .flags =
          (selection.contains_windows
               ? imageformat::TsumugiManifestFlags::source_contains_windows
               : imageformat::TsumugiManifestFlags::none) |
          (!selection.whole_disk
               ? imageformat::TsumugiManifestFlags::partition_selection
               : imageformat::TsumugiManifestFlags::none),
      .source_disk_size = session.size_bytes(),
      .logical_sector_size = session.logical_sector_size(),
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .source_model_hash = hashes.model,
      .source_serial_hash = hashes.serial,
      .source_state_hash = hashes.state,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
      .partition_snapshot = std::move(partition_snapshot),
  };
  auto partitions = layout.partitions;
  std::sort(
      partitions.begin(), partitions.end(), [](const auto& left, const auto& right) {
        return left.first_lba < right.first_lba;
      });
  PreparedExactImage result{.manifest = std::move(manifest)};
  result.manifest.partitions.reserve(partitions.size());
  for (const auto& item : partitions) {
    if (item.entry_index == (std::numeric_limits<std::uint32_t>::max)() ||
        item.last_lba < item.first_lba) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPTパーティション",
          L"GPTパーティション番号または範囲が不正です");
    }
    std::uint64_t offset{};
    std::uint64_t sectors{};
    std::uint64_t length{};
    if (!checked_multiply(
            item.first_lba, layout.logical_sector_size, offset) ||
        !checked_add(item.last_lba - item.first_lba, 1U, sectors) ||
        !checked_multiply(sectors, layout.logical_sector_size, length)) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPTパーティション範囲",
          L"GPTパーティション範囲が64bit上限を超えます");
    }
    auto reviewed = find_reviewed_partition_by_range(
        source.observed.observed,
        offset,
        length,
        L"PE Tsumugi GPT PartitionNumber対応");
    if (!reviewed) {
      return clonecore::Result<PreparedExactImage>::failure(
          reviewed.error());
    }
    const auto partition_number = reviewed.value()->number;
    const bool selected = diskmodel::image_partition_selection_contains(
        selection, partition_number);
    const bool required = selected &&
        partition_required(selection, partition_number);
    auto flags = selected
        ? imageformat::TsumugiManifestPartitionFlags::selected
        : imageformat::TsumugiManifestPartitionFlags::none;
    if (required) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::required;
    }
    auto role = gpt_role(item.type_guid);
    if (item.type_guid == clonecore::gpt_type_basic_data() && required &&
        selection.contains_windows) {
      role = imageformat::TsumugiManifestPartitionRole::windows;
      flags = flags |
          imageformat::TsumugiManifestPartitionFlags::contains_windows;
    }
    imageformat::TsumugiManifestPartition partition{
        .source_table_index = item.entry_index + 1U,
        .source_partition_number = partition_number,
        .role = role,
        .file_system = gpt_file_system(item.type_guid),
        .flags = flags,
        .source_offset = offset,
        .source_size = length,
        .used_bytes = selected ? length : 0U,
        .minimum_target_bytes = selected ? length : 0U,
        .planned_target_bytes = selected ? length : 0U,
        .payload_logical_offset = selected ? offset : 0U,
        .payload_logical_length = selected ? length : 0U,
        .type_id = item.type_guid.bytes,
        .unique_id = item.unique_guid.bytes,
    };
    result.manifest.partitions.push_back(std::move(partition));
    if (!selected) {
      continue;
    }
    if (!checked_add(
            result.logical_payload_bytes,
            length,
            result.logical_payload_bytes) ||
        result.selected_partition_count ==
            (std::numeric_limits<std::uint32_t>::max)()) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi GPT選択容量",
          L"選択済みGPTパーティションの容量または件数が上限を超えます");
    }
    ++result.selected_partition_count;
    const auto appended = append_partition_chunks(
        result.chunks, offset, length, session);
    if (!appended) {
      return clonecore::Result<PreparedExactImage>::failure(
          appended.error());
    }
  }
  return clonecore::Result<PreparedExactImage>::success(std::move(result));
}

clonecore::Result<PreparedExactImage> prepare_mbr_image(
    const DirectImageCreateRequest& request,
    const diskmodel::ReadOnlyPhysicalDiskHandle& source,
    const clonecore::MbrDisk& layout,
    const diskmodel::ImagePartitionSelection& selection,
    std::vector<std::byte> partition_snapshot,
    const SourceIdentityHashes& hashes,
    const imageformat::ITsumugiImageSourceSession& session) {
  imageformat::TsumugiManifest manifest{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style = imageformat::TsumugiManifestPartitionStyle::mbr,
      .flags =
          (selection.contains_windows
               ? imageformat::TsumugiManifestFlags::source_contains_windows
               : imageformat::TsumugiManifestFlags::none) |
          (!selection.whole_disk
               ? imageformat::TsumugiManifestFlags::partition_selection
               : imageformat::TsumugiManifestFlags::none),
      .source_disk_size = session.size_bytes(),
      .logical_sector_size = session.logical_sector_size(),
      .physical_sector_size = source.observed.observed.physical_sector_size,
      .source_model_hash = hashes.model,
      .source_serial_hash = hashes.serial,
      .source_state_hash = hashes.state,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
      .partition_snapshot = std::move(partition_snapshot),
  };
  auto partitions = layout.partitions;
  std::sort(
      partitions.begin(), partitions.end(), [](const auto& left, const auto& right) {
        return left.first_lba < right.first_lba;
      });
  PreparedExactImage result{.manifest = std::move(manifest)};
  result.manifest.partitions.reserve(partitions.size());
  for (const auto& item : partitions) {
    std::uint64_t offset{};
    std::uint64_t length{};
    if (!checked_multiply(
            item.first_lba, layout.logical_sector_size, offset) ||
        !checked_multiply(
            item.sector_count, layout.logical_sector_size, length)) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi MBRパーティション範囲",
          L"MBRパーティション範囲が64bit上限を超えます");
    }
    auto reviewed = find_reviewed_partition_by_range(
        source.observed.observed,
        offset,
        length,
        L"PE Tsumugi MBR PartitionNumber対応");
    if (!reviewed) {
      return clonecore::Result<PreparedExactImage>::failure(
          reviewed.error());
    }
    const auto partition_number = reviewed.value()->number;
    const bool selected = diskmodel::image_partition_selection_contains(
        selection, partition_number);
    const bool required = selected &&
        partition_required(selection, partition_number);
    auto flags = selected
        ? imageformat::TsumugiManifestPartitionFlags::selected
        : imageformat::TsumugiManifestPartitionFlags::none;
    if (required) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::required;
    }
    if (item.active) {
      flags = flags | imageformat::TsumugiManifestPartitionFlags::active;
    }
    auto role = imageformat::TsumugiManifestPartitionRole::data;
    if (item.type == 0x27U) {
      role = imageformat::TsumugiManifestPartitionRole::recovery;
    } else if (item.type == 0x07U && required &&
               selection.contains_windows) {
      role = imageformat::TsumugiManifestPartitionRole::windows;
      flags = flags |
          imageformat::TsumugiManifestPartitionFlags::contains_windows;
    }
    imageformat::TsumugiManifestPartition partition{
        .source_table_index = static_cast<std::uint32_t>(item.table_index) + 1U,
        .source_partition_number = partition_number,
        .role = role,
        .file_system = imageformat::TsumugiManifestFileSystem::unknown,
        .flags = flags,
        .source_offset = offset,
        .source_size = length,
        .used_bytes = selected ? length : 0U,
        .minimum_target_bytes = selected ? length : 0U,
        .planned_target_bytes = selected ? length : 0U,
        .payload_logical_offset = selected ? offset : 0U,
        .payload_logical_length = selected ? length : 0U,
    };
    partition.type_id[0] = static_cast<std::byte>(item.type);
    result.manifest.partitions.push_back(std::move(partition));
    if (!selected) {
      continue;
    }
    if (!checked_add(
            result.logical_payload_bytes,
            length,
            result.logical_payload_bytes) ||
        result.selected_partition_count ==
            (std::numeric_limits<std::uint32_t>::max)()) {
      return failure<PreparedExactImage>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Tsumugi MBR選択容量",
          L"選択済みMBRパーティションの容量または件数が上限を超えます");
    }
    ++result.selected_partition_count;
    const auto appended = append_partition_chunks(
        result.chunks, offset, length, session);
    if (!appended) {
      return clonecore::Result<PreparedExactImage>::failure(
          appended.error());
    }
  }
  return clonecore::Result<PreparedExactImage>::success(std::move(result));
}

clonecore::Result<std::uint64_t> maximum_image_bytes(
    const PreparedExactImage& plan,
    const bool rescue_mode) {
  auto manifest = imageformat::build_tsumugi_manifest_v1(plan.manifest);
  if (!manifest) {
    return clonecore::Result<std::uint64_t>::failure(manifest.error());
  }
  std::uint64_t records{};
  std::uint64_t total = imageformat::kTsumugiHeaderSize;
  const std::uint64_t record_count = rescue_mode
      ? imageformat::kTsumugiMaximumChunkCount
      : static_cast<std::uint64_t>(plan.chunks.size());
  if (!checked_multiply(
          record_count, imageformat::kTsumugiChunkRecordSize,
          records) ||
      !checked_add(total, imageformat::kTsumugiMetadataHeaderSize, total) ||
      !checked_add(total, manifest.value().size(), total) ||
      !checked_add(total, records, total) ||
      !checked_add(total, plan.logical_payload_bytes, total) ||
      !checked_add(total, imageformat::kTsumugiFooterSize, total)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"PE Tsumugi最大画像寸法",
        L"画像の最大寸法が64bit上限を超えます");
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

clonecore::Status validate_request(
    const DirectImageCreateRequest& request,
    const DirectImageCreateDependencies& dependencies) {
  if (!ends_with_tsumugi(request.final_path) ||
      request.created_utc.empty() || request.app_version.empty() ||
      !imageformat::is_supported_tsumugi_create_verification_mode(
          request.verification_mode) ||
      (request.selected_source.partition_style !=
           diskmodel::PartitionStyle::gpt &&
       request.selected_source.partition_style !=
           diskmodel::PartitionStyle::mbr)) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE直接Tsumugi作成要求",
        L"GPT/MBRコピー元、絶対.tsumugiパス、作成日時、アプリ版が必要です"));
  }
  if (request.rescue_mode &&
      request.selected_source.logical_sector_size != 512U) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE救出Tsumugiの論理セクター",
        L"実媒体検証が完了するまで、救出イメージ作成は512バイト論理セクターだけに限定します"));
  }
  if (request.rescue_mode && !request.selected_partition_numbers.empty()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE救出Tsumugiのpartition選択",
        L"救出イメージはディスク全体だけを対象とし、部分選択は通常exactモードで実行してください"));
  }
  const auto selection = diskmodel::normalize_image_partition_selection(
      request.selected_source,
      request.selected_partition_numbers);
  if (!selection) {
    return clonecore::Status::failure(selection.error());
  }
  if (!dependencies.set_source_read_only ||
      !dependencies.open_read_only_source ||
      !dependencies.query_destination_file_system ||
      !dependencies.validate_destination ||
      (request.rescue_mode && !dependencies.make_rescue_staging)) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE直接Tsumugi依存境界",
        L"コピー元保護、読取り専用Source、または保存先検証境界がありません"));
  }
  return clonecore::success_status();
}

clonecore::DiskOperationProgress translate_rescue_progress(
    const clonecore::RescueCopyProgress& progress) {
  clonecore::DiskOperationStage stage =
      clonecore::DiskOperationStage::copying_data;
  if (progress.phase == clonecore::RescueCopyPhase::validating) {
    stage = clonecore::DiskOperationStage::verifying_source;
  } else if (progress.phase == clonecore::RescueCopyPhase::flushing) {
    stage = clonecore::DiskOperationStage::flushing_data;
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

clonecore::Result<imageformat::TsumugiImageStorageFileSystem>
query_destination_file_system_with_windows_apis(const std::wstring& path) {
  std::vector<wchar_t> full(32768U, L'\0');
  const DWORD length = GetFullPathNameW(
      path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Tsumugi保存先絶対パス取得",
            length == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  std::array<wchar_t, MAX_PATH + 1U> root{};
  if (!GetVolumePathNameW(
          full.data(), root.data(), static_cast<DWORD>(root.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Tsumugi保存先Volume取得",
            GetLastError()));
  }
  std::array<wchar_t, 32U> file_system{};
  if (!GetVolumeInformationW(
          root.data(), nullptr, 0U, nullptr, nullptr, nullptr,
          file_system.data(), static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"PE Tsumugi保存先ファイルシステム取得",
            GetLastError()));
  }
  if (_wcsicmp(file_system.data(), L"NTFS") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::ntfs);
  }
  if (_wcsicmp(file_system.data(), L"exFAT") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::exfat);
  }
  return failure<imageformat::TsumugiImageStorageFileSystem>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"PE Tsumugi保存先ファイルシステム",
      L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
}

template <std::size_t Size>
bool digest_all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

bool same_resume_identities(
    const operationcore::ResumeIdentityBinding& left,
    const operationcore::ResumeIdentityBinding& right) noexcept {
  return left.source_identity_hash == right.source_identity_hash &&
      left.target_identity_hash == right.target_identity_hash &&
      left.output_identity_hash == right.output_identity_hash;
}

bool same_resume_binding(
    const operationcore::ResumeSlotBinding& left,
    const operationcore::ResumeSlotBinding& right) noexcept {
  if (left.capability != right.capability ||
      left.operation_id != right.operation_id ||
      !same_resume_identities(left.identities, right.identities) ||
      left.checkpoint_record_hash != right.checkpoint_record_hash ||
      left.partial_file_object_identity_hash !=
          right.partial_file_object_identity_hash ||
      left.owned_object_file_bindings.size() !=
          right.owned_object_file_bindings.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < left.owned_object_file_bindings.size(); ++index) {
    if (left.owned_object_file_bindings[index].role !=
            right.owned_object_file_bindings[index].role ||
        left.owned_object_file_bindings[index].file_object_identity_hash !=
            right.owned_object_file_bindings[index]
                .file_object_identity_hash) {
      return false;
    }
  }
  return true;
}

bool same_create_storage_proof(
    const DirectImageCreateResumeStorageProof& left,
    const DirectImageCreateResumeStorageProof& right) noexcept {
  return left.all_identities_from_open_handles &&
      right.all_identities_from_open_handles &&
      left.checkpoint_storage_identity_hash ==
          right.checkpoint_storage_identity_hash &&
      left.source_storage_identity_hash ==
          right.source_storage_identity_hash &&
      left.destination_storage_identity_hash ==
          right.destination_storage_identity_hash;
}

clonecore::Result<operationcore::Sha256Digest>
hash_image_create_source_identity(
    const clonecore::StableDiskIdentity& source) {
  std::vector<std::byte> material;
  material.reserve(256U);
  append_domain(material, "YTEC-PE-IMAGE-CREATE-SOURCE-IDENTITY-V1");
  const auto model = append_utf16(
      material, source.model, L"PE image-create source identity");
  const auto serial = append_ascii(
      material,
      source.serial_suffix,
      L"PE image-create source identity");
  const auto instance = append_utf16(
      material,
      source.device_instance_id,
      L"PE image-create source identity");
  if (!model || !serial || !instance) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        !model ? model.error() : !serial ? serial.error() : instance.error());
  }
  append_u64(material, source.size_bytes);
  append_u32(material, source.logical_sector_size);
  material.push_back(source.is_system_disk ? std::byte{1} : std::byte{0});
  return imageformat::sha256(material);
}

clonecore::Result<operationcore::Sha256Digest>
hash_image_create_output_identity(
    const std::wstring& final_path,
    const operationcore::Sha256Digest& manifest_hash,
    const operationcore::Sha256Digest& source_state_hash,
    const DirectImageCreateResumeStorageProof& storage,
    const std::wstring& continuity_token) {
  std::vector<std::byte> material;
  material.reserve(1024U + final_path.size() * sizeof(wchar_t) +
                   continuity_token.size() * sizeof(wchar_t));
  append_domain(material, "YTEC-PE-IMAGE-CREATE-OUTPUT-IDENTITY-V1");
  const auto final = append_utf16(
      material, final_path, L"PE image-create output identity");
  const auto continuity = append_utf16(
      material,
      continuity_token,
      L"PE image-create output continuity");
  if (!final || !continuity) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        final ? continuity.error() : final.error());
  }
  append_array(material, manifest_hash);
  append_array(material, source_state_hash);
  append_array(material, storage.checkpoint_storage_identity_hash);
  append_array(material, storage.source_storage_identity_hash);
  append_array(material, storage.destination_storage_identity_hash);
  return imageformat::sha256(material);
}

clonecore::Result<operationcore::Sha256Digest>
hash_image_create_immutable_payload(
    const operationcore::ResumeIdentityBinding& identities,
    const operationcore::Sha256Digest& source_state_hash,
    const imageformat::TsumugiManifest& manifest,
    const std::span<const std::byte> manifest_bytes,
    const std::span<const imageformat::TsumugiStreamBuildChunk> chunks,
    const std::wstring& final_path,
    const std::wstring& continuity_token,
    const imageformat::TsumugiCreateVerificationMode verification_mode,
    const bool encrypted) {
  std::vector<std::byte> material;
  material.reserve(
      1024U + manifest_bytes.size() +
      chunks.size() * (sizeof(std::uint64_t) * 3U + sizeof(std::uint32_t)));
  append_domain(material, "YTEC-PE-IMAGE-CREATE-IMMUTABLE-PLAN-V1");
  append_array(material, identities.source_identity_hash);
  append_array(material, identities.target_identity_hash);
  append_array(material, identities.output_identity_hash);
  append_array(material, source_state_hash);
  const auto final = append_utf16(
      material, final_path, L"PE image-create immutable final path");
  const auto continuity = append_utf16(
      material,
      continuity_token,
      L"PE image-create immutable continuity");
  if (!final || !continuity) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        final ? continuity.error() : final.error());
  }
  append_u64(material, manifest.source_disk_size);
  append_u32(material, manifest.logical_sector_size);
  append_u32(material, manifest.physical_sector_size);
  append_u32(material, kDirectImageChunkBytes);
  material.push_back(static_cast<std::byte>(verification_mode));
  material.push_back(encrypted ? std::byte{1} : std::byte{0});
  append_u64(material, static_cast<std::uint64_t>(manifest_bytes.size()));
  material.insert(
      material.end(), manifest_bytes.begin(), manifest_bytes.end());
  append_u64(material, static_cast<std::uint64_t>(chunks.size()));
  for (const auto& chunk : chunks) {
    append_u64(material, chunk.logical_offset);
    append_u64(material, chunk.logical_length);
    append_u64(material, chunk.source_offset);
    append_u32(material, static_cast<std::uint32_t>(chunk.flags));
  }
  return imageformat::sha256(material);
}

operationcore::CheckpointPhase checkpoint_phase(
    const imageformat::TsumugiCreateResumePhaseV1 phase) noexcept {
  switch (phase) {
    case imageformat::TsumugiCreateResumePhaseV1::preparing:
      return operationcore::CheckpointPhase::preparing;
    case imageformat::TsumugiCreateResumePhaseV1::prepared:
      return operationcore::CheckpointPhase::prepared;
    case imageformat::TsumugiCreateResumePhaseV1::commit_ready:
      return operationcore::CheckpointPhase::commit_ready;
  }
  return operationcore::CheckpointPhase::preparing;
}

clonecore::Result<imageformat::TsumugiCreateResumeProgressV1>
resume_progress_from_checkpoint(
    const operationcore::InterruptionCheckpoint& checkpoint) {
  if (checkpoint.schema_version !=
          operationcore::kCheckpointSchemaVersionV3 ||
      !checkpoint.output_progress_evidence) {
    return failure<imageformat::TsumugiCreateResumeProgressV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_REVISION_MISMATCH,
        L"PE image-create checkpoint progress",
        L"schema v3出力進捗証跡がありません");
  }
  imageformat::TsumugiCreateResumePhaseV1 phase{};
  switch (checkpoint.phase) {
    case operationcore::CheckpointPhase::preparing:
      phase = imageformat::TsumugiCreateResumePhaseV1::preparing;
      break;
    case operationcore::CheckpointPhase::prepared:
      phase = imageformat::TsumugiCreateResumePhaseV1::prepared;
      break;
    case operationcore::CheckpointPhase::commit_ready:
      phase = imageformat::TsumugiCreateResumePhaseV1::commit_ready;
      break;
    case operationcore::CheckpointPhase::executing:
    case operationcore::CheckpointPhase::verifying:
      return failure<imageformat::TsumugiCreateResumeProgressV1>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_STATE,
          L"PE image-create checkpoint phase",
          L"schema v3 image-createはpreparing/prepared/commit-readyだけを受理します");
  }
  return clonecore::Result<
      imageformat::TsumugiCreateResumeProgressV1>::success({
      .phase = phase,
      .verified_logical_bytes = checkpoint.verified_work_bytes,
      .verified_chunk_count = checkpoint.verified_chunk_count,
      .primary_output_length =
          checkpoint.output_progress_evidence->primary_output_length,
      .journal_length =
          checkpoint.output_progress_evidence->journal_length,
      .verified_prefix_hash =
          checkpoint.output_progress_evidence->verified_prefix_hash,
  });
}

operationcore::InterruptionCheckpoint make_image_create_checkpoint(
    const operationcore::OperationPlan& plan,
    const operationcore::Sha256Digest& plan_hash,
    const operationcore::ResumeIdentityBinding& identities,
    const std::wstring& continuity_token,
    const imageformat::TsumugiCreateResumeProgressV1& progress,
    const std::uint64_t revision) {
  return operationcore::InterruptionCheckpoint{
      .schema_version = operationcore::kCheckpointSchemaVersionV3,
      .operation_id = plan.operation_id,
      .kind = plan.kind,
      .environment = plan.environment,
      .phase = checkpoint_phase(progress.phase),
      .revision = revision,
      .expected_work_bytes = plan.expected_work_bytes,
      .verified_work_bytes = progress.verified_logical_bytes,
      .verified_chunk_count = progress.verified_chunk_count,
      .plan_hash = plan_hash,
      .output_identity_hash = identities.output_identity_hash,
      .source = plan.source,
      .target = std::nullopt,
      .continuity_token = continuity_token,
      .preparation_evidence = std::nullopt,
      .output_progress_evidence =
          operationcore::CheckpointOutputProgressEvidence{
              .verified_prefix_hash = progress.verified_prefix_hash,
              .primary_output_length = progress.primary_output_length,
              .journal_length = progress.journal_length,
              .auxiliary_output_length = 0U,
          },
  };
}

clonecore::Result<operationcore::ParsedCheckpoint> parse_own_checkpoint(
    const operationcore::InterruptionCheckpoint& checkpoint) {
  auto bytes = operationcore::serialize_checkpoint(checkpoint);
  return bytes
      ? operationcore::parse_checkpoint(bytes.value())
      : clonecore::Result<operationcore::ParsedCheckpoint>::failure(
            bytes.error());
}

}  // namespace

clonecore::Result<DirectImageCreateReport> execute_direct_image_create(
    const DirectImageCreateRequest& request,
    const DirectImageCreateDependencies& dependencies) {
  const auto valid = validate_request(request, dependencies);
  if (!valid) {
    return clonecore::Result<DirectImageCreateReport>::failure(valid.error());
  }
  auto reviewed_selection = diskmodel::normalize_image_partition_selection(
      request.selected_source,
      request.selected_partition_numbers);
  if (!reviewed_selection) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        reviewed_selection.error());
  }
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"PE直接Tsumugi開始前",
        L"開始前に取り消されました");
  }
  auto expected = diskmodel::make_stable_disk_identity(
      request.selected_source, request.selected_source.is_system_disk);
  if (!expected) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        expected.error());
  }

  imageformat::WindowsTsumugiDestinationGuardRequest destination_guard{
      .final_path = request.final_path,
      .expected_source_disk = expected.value(),
      .required_available_bytes = 1U,
      .replace_existing = request.replace_existing,
  };
  auto destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        destination.error());
  }
  auto storage = dependencies.query_destination_file_system(
      request.final_path);
  if (!storage) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        storage.error());
  }
  if (storage.value() != imageformat::TsumugiImageStorageFileSystem::ntfs &&
      storage.value() != imageformat::TsumugiImageStorageFileSystem::exfat) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE Tsumugi保存先ファイルシステム",
        L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます");
  }
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"PEコピー元read-only化前",
        L"コピー元を変更する前に取り消されました");
  }

  const auto protected_source = dependencies.set_source_read_only(
      expected.value(), true);
  if (!protected_source) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        protected_source.error());
  }
  auto source = dependencies.open_read_only_source(expected.value());
  if (!source) {
    return clonecore::Result<DirectImageCreateReport>::failure(source.error());
  }
  if (!source.value().reader ||
      !source.value().observed.observed.read_only.has_value() ||
      !source.value().observed.observed.read_only.value() ||
      !same_reviewed_layout(
          request.selected_source, source.value().observed.observed)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Tsumugiコピー元の実行直前再識別",
        L"コピー元のread-only状態、パーティション形式、またはレビュー済みレイアウトが一致しません");
  }
  auto observed_selection = diskmodel::normalize_image_partition_selection(
      source.value().observed.observed,
      request.selected_partition_numbers);
  if (!observed_selection ||
      !same_image_partition_selection(
          reviewed_selection.value(), observed_selection.value())) {
    return observed_selection
        ? failure<DirectImageCreateReport>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"PE Tsumugi partition選択再証明",
              L"レビュー後に選択領域または必須Windows領域のbindingが変化しました")
        : clonecore::Result<DirectImageCreateReport>::failure(
              observed_selection.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      expected.value(), source.value().observed.identity,
      L"PE Tsumugiコピー元");
  if (!identity) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        identity.error());
  }

  const auto table_style =
      source.value().observed.observed.partition_style ==
              diskmodel::PartitionStyle::gpt
          ? imageformat::PartitionTableStyle::gpt
          : imageformat::PartitionTableStyle::mbr;
  auto partition_snapshot = imageformat::capture_partition_snapshot_v1(
      *source.value().reader, table_style);
  if (!partition_snapshot) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        partition_snapshot.error());
  }
  auto hashes = make_source_hashes(
      source.value().observed.identity,
      source.value().observed.observed.physical_sector_size,
      partition_snapshot.value());
  if (!hashes) {
    return clonecore::Result<DirectImageCreateReport>::failure(hashes.error());
  }

  LockedSourceSession session(
      std::move(source.value().reader), hashes.value());
  clonecore::Result<PreparedExactImage> plan =
      failure<PreparedExactImage>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"PE Tsumugiパーティション形式",
          L"コピー元はGPTまたはMBRでなければなりません");
  if (table_style == imageformat::PartitionTableStyle::gpt) {
    auto parsed = clonecore::parse_gpt(session);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    const auto reviewed = validate_gpt_inventory_geometry(
        source.value().observed.observed, parsed.value());
    if (!reviewed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          reviewed.error());
    }
    plan = prepare_gpt_image(
        request, source.value(), parsed.value(),
        observed_selection.value(),
        partition_snapshot.value(), hashes.value(), session);
  } else {
    auto parsed = clonecore::parse_mbr(session);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    const auto reviewed = validate_mbr_inventory_geometry(
        source.value().observed.observed, parsed.value());
    if (!reviewed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          reviewed.error());
    }
    plan = prepare_mbr_image(
        request, source.value(), parsed.value(),
        observed_selection.value(),
        partition_snapshot.value(), hashes.value(), session);
  }
  if (!plan) {
    return clonecore::Result<DirectImageCreateReport>::failure(plan.error());
  }
  if (request.rescue_mode) {
    plan.value().manifest.mode = imageformat::TsumugiManifestMode::rescue;
  }
  auto maximum_bytes = maximum_image_bytes(
      plan.value(), request.rescue_mode);
  if (!maximum_bytes) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        maximum_bytes.error());
  }
  std::uint64_t required_available_bytes = maximum_bytes.value();
  if (request.rescue_mode &&
      !checked_add(
          required_available_bytes,
          source.value().observed.identity.size_bytes,
          required_available_bytes)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"PE救出Tsumugi必要容量",
        L"RAW一時領域と最大画像寸法の合計が64bit上限を超えます");
  }
  destination_guard.required_available_bytes = required_available_bytes;
  destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        destination.error());
  }

  if (request.rescue_mode) {
    auto staging = dependencies.make_rescue_staging(
        imageformat::WindowsTsumugiRescueStagingRequest{
            .final_path = request.final_path,
            .expected_source_disk = source.value().observed.identity,
            .source_disk_size = source.value().observed.identity.size_bytes,
            .logical_sector_size =
                source.value().observed.identity.logical_sector_size,
            .source_model_hash = hashes.value().model,
            .source_serial_hash = hashes.value().serial,
            .source_state_hash = hashes.value().state,
            .required_available_bytes = required_available_bytes,
            .replace_existing = request.replace_existing,
        });
    if (!staging) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          staging.error());
    }
    clonecore::RescueCopyCallbacks rescue_callbacks{
        .cancellation_requested = request.callbacks.cancellation_requested,
        .safe_boundary = request.callbacks.safe_boundary,
    };
    if (request.callbacks.progress) {
      rescue_callbacks.progress =
          [&callbacks = request.callbacks](
              const clonecore::RescueCopyProgress& progress) {
            clonecore::report_disk_operation_progress(
                callbacks, translate_rescue_progress(progress));
          };
    }
    imageformat::TsumugiImageCreateRequest rescue_image{
        .final_path = request.final_path,
        .storage_file_system = storage.value(),
        .manifest = plan.value().manifest,
        .compression = imageformat::ImageCompression::zstandard,
        .chunk_size = kDirectImageChunkBytes,
        .verification_block_bytes = 4U * 1024U * 1024U,
        .verification_mode = request.verification_mode,
        .replace_existing = request.replace_existing,
    };
    if (request.encryption_password.has_value()) {
      rescue_image.encryption =
          imageformat::TsumugiImageEncryptionRequest{
              .password = *request.encryption_password,
          };
    }
    auto created = imageformat::create_tsumugi_rescue_image_v1(
        imageformat::TsumugiRescueImageCreateRequest{
            .image = std::move(rescue_image),
            .rescue_copy = clonecore::RescueRawCopyRequest{
                .environment =
                    clonecore::RescueExecutionEnvironment::winpe,
                .source_kind = source.value().observed.identity.is_system_disk
                    ? clonecore::RescueSourceKind::system_disk
                    : clonecore::RescueSourceKind::data_disk,
                .rescue_mode_explicitly_confirmed = true,
                .large_block_bytes = 4U * 1024U * 1024U,
                .callbacks = std::move(rescue_callbacks),
            },
            .failing_source = &session,
            .staging = staging.value().get(),
        },
        request.callbacks);
    if (!created) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          created.error());
    }
    if (!imageformat::selected_tsumugi_creation_verification_passed(
            created.value().image) ||
        !created.value().image.stream.committed ||
        !created.value().staging_sealed_for_image_read ||
        !created.value().staging_discarded_before_final_commit ||
        !created.value()
             .staging_destination_revalidated_before_final_commit) {
      return failure<DirectImageCreateReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"PE救出Tsumugi作成結果",
          L"救出一時領域の封印・破棄・保存先再識別、選択済み画像検証、または完成名確定を確認できません");
    }
    return clonecore::Result<DirectImageCreateReport>::success({
        .source_identity = source.value().observed.identity,
        .source_partition_style =
            source.value().observed.observed.partition_style,
        .imaged_partition_count = plan.value().selected_partition_count,
        .logical_payload_bytes = plan.value().logical_payload_bytes,
        .source_read_only_verified = true,
        .source_left_read_only = true,
        .layout_revalidated_before_commit = false,
        .rescue_mode = true,
        .rescue = std::move(created.value().rescue),
        .image = std::move(created.value().image),
    });
  }

  imageformat::TsumugiImageCreateRequest create_request{
      .final_path = request.final_path,
      .storage_file_system = storage.value(),
      .manifest = plan.value().manifest,
      .chunks = plan.value().chunks,
      .compression = imageformat::ImageCompression::zstandard,
      .chunk_size = kDirectImageChunkBytes,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .verification_mode = request.verification_mode,
      .replace_existing = request.replace_existing,
      .source_session = &session,
  };
  if (request.encryption_password.has_value()) {
    create_request.encryption = imageformat::TsumugiImageEncryptionRequest{
        .password = *request.encryption_password,
    };
  }
  auto staged = imageformat::prepare_tsumugi_image_v1(
      create_request, request.callbacks);
  if (!staged) {
    return clonecore::Result<DirectImageCreateReport>::failure(staged.error());
  }

  auto final_snapshot = imageformat::capture_partition_snapshot_v1(
      session, table_style);
  if (!final_snapshot || final_snapshot.value() != partition_snapshot.value()) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        final_snapshot
            ? image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_CRC,
                  L"PE Tsumugiコピー元レイアウト最終確認",
                  L"read-only Sourceのパーティション表が作成開始時から変化しました")
            : final_snapshot.error());
  }

  destination_guard.phase = imageformat::
      WindowsTsumugiDestinationGuardPhase::before_commit_owned_partial;
  destination_guard.expected_owned_partial_bytes =
      staged.value().report().stream.image_length;
  destination = dependencies.validate_destination(destination_guard);
  if (!destination) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        destination.error());
  }
  auto committed = staged.value().commit_verified();
  if (!committed) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        committed.error());
  }
  const auto& image = committed.value();
  if (!imageformat::selected_tsumugi_creation_verification_passed(image) ||
      !image.stream.committed || !image.unreadable_ranges.empty() ||
      image.stream.chunk_count != plan.value().chunks.size()) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE Tsumugi作成結果",
        L"選択済み作成時検証、チャンク件数、欠損なし、または完成名確定を確認できません");
  }
  return clonecore::Result<DirectImageCreateReport>::success({
      .source_identity = source.value().observed.identity,
      .source_partition_style =
          source.value().observed.observed.partition_style,
      .imaged_partition_count = plan.value().selected_partition_count,
      .logical_payload_bytes = plan.value().logical_payload_bytes,
      .source_read_only_verified = true,
      .source_left_read_only = true,
      .layout_revalidated_before_commit = true,
      .rescue_mode = false,
      .image = committed.take_value(),
  });
}

clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_resume_v1(
    const DirectImageCreateRequest& request,
    const DirectImageCreateResumeCommand& command,
    const DirectImageCreateResumeDependencies& dependencies) {
  if (command.action == DirectImageCreateResumeAction::cancel) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"PE image-create persistent resume",
        L"再開または新規開始を選択せず取り消しました");
  }
  const auto valid = validate_request(request, dependencies.direct);
  if (!valid) {
    return clonecore::Result<DirectImageCreateReport>::failure(valid.error());
  }
  auto reviewed_selection = diskmodel::normalize_image_partition_selection(
      request.selected_source,
      request.selected_partition_numbers);
  if (!reviewed_selection) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        reviewed_selection.error());
  }
  if (request.rescue_mode || request.replace_existing ||
      dependencies.slot_platform == nullptr ||
      !dependencies.prove_storage_separation ||
      (command.action == DirectImageCreateResumeAction::start_new &&
       !dependencies.make_bound_slot_platform) ||
      (command.action == DirectImageCreateResumeAction::start_new &&
       digest_all_zero(command.new_operation_id)) ||
      (command.action == DirectImageCreateResumeAction::resume_existing &&
       !command.reviewed_existing_slot)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE image-create persistent契約",
        L"通常exact・create-new、slot platform、storage proof、および操作bindingが必要です");
  }
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"PE image-create persistent開始前",
        L"Sourceまたはowned outputを変更する前に取り消されました");
  }

  auto observed = dependencies.slot_platform
      ->inspect_persistent_pe_exact_image_create();
  if (!observed) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        observed.error());
  }
  const bool start_new =
      command.action == DirectImageCreateResumeAction::start_new;
  if ((start_new && observed.value().state != operationcore::
                        PersistentPeExactImageCreateObjectState::no_slot) ||
      (!start_new &&
       observed.value().state != operationcore::
           PersistentPeExactImageCreateObjectState::staged &&
       observed.value().state != operationcore::
           PersistentPeExactImageCreateObjectState::published &&
       observed.value().state != operationcore::
           PersistentPeExactImageCreateObjectState::retirement_pending)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::access_denied,
        ERROR_BUSY,
        L"PE image-create persistent slot admission",
        L"空slotへの新規開始、またはcapability-8の完全binding再開だけを許可します");
  }
  if (!start_new &&
      (!observed.value().slot || !observed.value().binding ||
       !same_resume_binding(
           *observed.value().binding,
           *command.reviewed_existing_slot))) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE image-create reviewed resume binding",
        L"表示後にcheckpoint、owned object、または状態が変化しました");
  }

  DirectImageCreateResumeContinuityV1 continuity;
  std::wstring continuity_token;
  if (start_new) {
    auto image_id = imageformat::generate_tsumugi_salt();
    if (!image_id) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          image_id.error());
    }
    continuity.created_utc = request.created_utc;
    continuity.app_version = request.app_version;
    continuity.verification_mode = request.verification_mode;
    continuity.encrypted = request.encryption_password.has_value();
    continuity.selected_partition_numbers =
        canonical_persisted_selection(reviewed_selection.value());
    continuity.image_id = image_id.take_value();
    if (continuity.encrypted) {
      auto salt = imageformat::generate_tsumugi_salt();
      auto nonce = imageformat::generate_tsumugi_nonce();
      if (!salt || !nonce) {
        return clonecore::Result<DirectImageCreateReport>::failure(
            salt ? nonce.error() : salt.error());
      }
      continuity.argon2.salt = salt.take_value();
      continuity.base_nonce = nonce.take_value();
    }
    auto built = build_direct_image_create_resume_continuity_v1(continuity);
    if (!built) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          built.error());
    }
    continuity_token = built.take_value();
  } else {
    auto parsed = parse_direct_image_create_resume_continuity_v1(
        observed.value().slot->checkpoint.checkpoint.continuity_token);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    continuity = parsed.take_value();
    continuity_token =
        observed.value().slot->checkpoint.checkpoint.continuity_token;
    if (continuity.encrypted != request.encryption_password.has_value() ||
        continuity.verification_mode != request.verification_mode ||
        continuity.selected_partition_numbers !=
            canonical_persisted_selection(reviewed_selection.value()) ||
        _wcsicmp(
            observed.value().final_path.c_str(),
            request.final_path.c_str()) != 0) {
      return failure<DirectImageCreateReport>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"PE image-create resume selection",
          L"暗号化・検証方式・保存先が前回のcanonical continuityと一致しません");
    }
  }
  if (continuity.encrypted) {
    const auto password = imageformat::assess_tsumugi_password(
        *request.encryption_password);
    if (!password.accepted) {
      return failure<DirectImageCreateReport>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PASSWORD,
          L"PE image-create resume password",
          L"再入力したパスワードはASCII印字文8文字以上でなければなりません");
    }
  }

  DirectImageCreateRequest effective = request;
  effective.created_utc = continuity.created_utc;
  effective.app_version = continuity.app_version;
  effective.selected_partition_numbers =
      continuity.selected_partition_numbers;
  auto storage_proof = dependencies.prove_storage_separation(
      effective,
      observed.value().slot);
  if (!storage_proof ||
      !storage_proof.value().all_identities_from_open_handles ||
      digest_all_zero(
          storage_proof.value().checkpoint_storage_identity_hash) ||
      digest_all_zero(storage_proof.value().source_storage_identity_hash) ||
      digest_all_zero(
          storage_proof.value().destination_storage_identity_hash) ||
      storage_proof.value().checkpoint_storage_identity_hash ==
          storage_proof.value().source_storage_identity_hash ||
      storage_proof.value().checkpoint_storage_identity_hash ==
          storage_proof.value().destination_storage_identity_hash ||
      storage_proof.value().source_storage_identity_hash ==
          storage_proof.value().destination_storage_identity_hash) {
    return storage_proof
        ? failure<DirectImageCreateReport>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"PE image-create storage proof",
              L"checkpoint、Source、保存先の3 opened-storage domainを分離できません")
        : clonecore::Result<DirectImageCreateReport>::failure(
              storage_proof.error());
  }
  const auto initial_storage = storage_proof.value();

  auto expected = diskmodel::make_stable_disk_identity(
      effective.selected_source,
      effective.selected_source.is_system_disk);
  if (!expected) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        expected.error());
  }
  imageformat::WindowsTsumugiDestinationGuardRequest destination_guard{
      .final_path = effective.final_path,
      .expected_source_disk = expected.value(),
      .required_available_bytes = 1U,
      .replace_existing = false,
  };
  const bool already_published = !start_new &&
      observed.value().state != operationcore::
          PersistentPeExactImageCreateObjectState::staged;
  if (!already_published) {
    const auto destination =
        dependencies.direct.validate_destination(destination_guard);
    if (!destination) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          destination.error());
    }
  }
  auto storage_file_system =
      dependencies.direct.query_destination_file_system(effective.final_path);
  if (!storage_file_system) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        storage_file_system.error());
  }
  if (storage_file_system.value() !=
          imageformat::TsumugiImageStorageFileSystem::ntfs &&
      storage_file_system.value() !=
          imageformat::TsumugiImageStorageFileSystem::exfat) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE image-create resume file system",
        L"永続再開画像はNTFSまたはexFATだけに保存できます");
  }
  const auto protected_source = dependencies.direct.set_source_read_only(
      expected.value(), true);
  if (!protected_source) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        protected_source.error());
  }
  auto source = dependencies.direct.open_read_only_source(expected.value());
  if (!source) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        source.error());
  }
  if (!source.value().reader ||
      !source.value().observed.observed.read_only.value_or(false) ||
      !same_reviewed_layout(
          effective.selected_source,
          source.value().observed.observed)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE image-create resume Source再識別",
        L"read-only状態またはレビュー済みパーティションレイアウトが一致しません");
  }
  auto observed_selection = diskmodel::normalize_image_partition_selection(
      source.value().observed.observed,
      effective.selected_partition_numbers);
  if (!observed_selection ||
      !same_image_partition_selection(
          reviewed_selection.value(), observed_selection.value())) {
    return observed_selection
        ? failure<DirectImageCreateReport>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"PE image-create resume partition選択再証明",
              L"continuityへ固定した選択領域または必須Windows領域のbindingが変化しました")
        : clonecore::Result<DirectImageCreateReport>::failure(
              observed_selection.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      expected.value(),
      source.value().observed.identity,
      L"PE image-create resume Source");
  if (!identity) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        identity.error());
  }
  const auto table_style =
      source.value().observed.observed.partition_style ==
              diskmodel::PartitionStyle::gpt
          ? imageformat::PartitionTableStyle::gpt
          : imageformat::PartitionTableStyle::mbr;
  auto partition_snapshot = imageformat::capture_partition_snapshot_v1(
      *source.value().reader, table_style);
  if (!partition_snapshot) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        partition_snapshot.error());
  }
  auto hashes = make_source_hashes(
      source.value().observed.identity,
      source.value().observed.observed.physical_sector_size,
      partition_snapshot.value());
  if (!hashes) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        hashes.error());
  }
  LockedSourceSession session(
      std::move(source.value().reader), hashes.value());
  clonecore::Result<PreparedExactImage> prepared_plan =
      failure<PreparedExactImage>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"PE image-create persistent partition style",
          L"SourceはGPTまたはMBRでなければなりません");
  if (table_style == imageformat::PartitionTableStyle::gpt) {
    auto parsed = clonecore::parse_gpt(session);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    const auto reviewed = validate_gpt_inventory_geometry(
        source.value().observed.observed, parsed.value());
    if (!reviewed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          reviewed.error());
    }
    prepared_plan = prepare_gpt_image(
        effective,
        source.value(),
        parsed.value(),
        observed_selection.value(),
        partition_snapshot.value(),
        hashes.value(),
        session);
  } else {
    auto parsed = clonecore::parse_mbr(session);
    if (!parsed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          parsed.error());
    }
    const auto reviewed = validate_mbr_inventory_geometry(
        source.value().observed.observed, parsed.value());
    if (!reviewed) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          reviewed.error());
    }
    prepared_plan = prepare_mbr_image(
        effective,
        source.value(),
        parsed.value(),
        observed_selection.value(),
        partition_snapshot.value(),
        hashes.value(),
        session);
  }
  if (!prepared_plan) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        prepared_plan.error());
  }
  auto maximum_bytes = maximum_image_bytes(prepared_plan.value(), false);
  if (!maximum_bytes) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        maximum_bytes.error());
  }
  destination_guard.required_available_bytes = maximum_bytes.value();
  if (!already_published) {
    const auto destination =
        dependencies.direct.validate_destination(destination_guard);
    if (!destination) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          destination.error());
    }
  }

  auto manifest_bytes = imageformat::build_tsumugi_manifest_v1(
      prepared_plan.value().manifest);
  if (!manifest_bytes) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        manifest_bytes.error());
  }
  auto manifest_hash = imageformat::sha256(manifest_bytes.value());
  auto source_identity_hash = hash_image_create_source_identity(
      source.value().observed.identity);
  if (!manifest_hash || !source_identity_hash) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        manifest_hash ? source_identity_hash.error() : manifest_hash.error());
  }
  operationcore::ResumeIdentityBinding identities{
      .source_identity_hash = source_identity_hash.value(),
      .target_identity_hash =
          initial_storage.destination_storage_identity_hash,
  };
  auto output_hash = hash_image_create_output_identity(
      effective.final_path,
      manifest_hash.value(),
      hashes.value().state,
      initial_storage,
      continuity_token);
  if (!output_hash) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        output_hash.error());
  }
  identities.output_identity_hash = output_hash.value();
  auto immutable = hash_image_create_immutable_payload(
      identities,
      hashes.value().state,
      prepared_plan.value().manifest,
      manifest_bytes.value(),
      prepared_plan.value().chunks,
      effective.final_path,
      continuity_token,
      continuity.verification_mode,
      continuity.encrypted);
  if (!immutable) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        immutable.error());
  }
  operationcore::OperationPlan operation_plan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = start_new
          ? command.new_operation_id
          : observed.value().slot->checkpoint.checkpoint.operation_id,
      .kind = operationcore::OperationKind::image_create,
      .environment = operationcore::OperationEnvironment::winpe,
      .source = source.value().observed.identity,
      .target = std::nullopt,
      .expected_work_bytes = prepared_plan.value().logical_payload_bytes,
      .immutable_payload_hash = immutable.value(),
  };
  auto plan_hash = operationcore::hash_operation_plan(operation_plan);
  if (!plan_hash) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        plan_hash.error());
  }

  std::optional<imageformat::TsumugiCreateResumeProgressV1>
      expected_progress;
  if (!start_new) {
    const auto checkpoint_valid = operationcore::validate_checkpoint_for_resume(
        observed.value().slot->checkpoint.checkpoint,
        operation_plan,
        operationcore::ReidentifiedOperation{
            .source = source.value().observed.identity,
            .target = std::nullopt,
        },
        continuity_token,
        identities.output_identity_hash);
    if (!checkpoint_valid ||
        !same_resume_identities(
            observed.value().slot->identities, identities)) {
      return checkpoint_valid
          ? failure<DirectImageCreateReport>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_FILE_INVALID,
                L"PE image-create durable identities",
                L"source、destinationまたはoutput identityがcheckpointと一致しません")
          : clonecore::Result<DirectImageCreateReport>::failure(
                checkpoint_valid.error());
    }
    auto progress = resume_progress_from_checkpoint(
        observed.value().slot->checkpoint.checkpoint);
    if (!progress) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          progress.error());
    }
    expected_progress = progress.take_value();
  }

  const imageformat::TsumugiCreateResumeBindingV1 stream_binding{
      .operation_id = operation_plan.operation_id,
      .plan_hash = plan_hash.value(),
      .source_identity_hash = identities.source_identity_hash,
      .source_state_hash = hashes.value().state,
      .destination_storage_identity_hash =
          initial_storage.destination_storage_identity_hash,
      .output_identity_hash = identities.output_identity_hash,
  };
  std::optional<imageformat::TsumugiEncryptionSettings> encryption;
  if (continuity.encrypted) {
    encryption = imageformat::TsumugiEncryptionSettings{
        .password = *effective.encryption_password,
        .argon2 = continuity.argon2,
        .base_nonce = continuity.base_nonce,
    };
  }
  imageformat::TsumugiStreamBuildRequest stream_request{
      .final_path = effective.final_path,
      .payload_kind = imageformat::TsumugiPayloadKind::exact_disk,
      .source_disk_size = prepared_plan.value().manifest.source_disk_size,
      .logical_sector_size =
          prepared_plan.value().manifest.logical_sector_size,
      .physical_sector_size =
          prepared_plan.value().manifest.physical_sector_size,
      .chunk_size = kDirectImageChunkBytes,
      .compression = imageformat::ImageCompression::zstandard,
      .verification_block_bytes = 4U * 1024U * 1024U,
      .verification_mode = continuity.verification_mode,
      .image_id = continuity.image_id,
      .manifest = manifest_bytes.value(),
      .chunks = prepared_plan.value().chunks,
      .encryption = encryption,
      .replace_existing = false,
  };

  operationcore::IResumeSlotPlatform* runtime_platform =
      dependencies.slot_platform;
  std::unique_ptr<operationcore::IResumeSlotPlatform> owned_platform;
  std::optional<operationcore::ResumeSlotRecord> runtime_record =
      observed.value().slot;
  std::optional<operationcore::ResumeSlotBinding> runtime_binding =
      observed.value().binding;

  const auto reprove_source_and_storage = [&]() -> clonecore::Status {
    auto current_snapshot = imageformat::capture_partition_snapshot_v1(
        session, table_style);
    if (!current_snapshot ||
        current_snapshot.value() != partition_snapshot.value()) {
      return clonecore::Status::failure(
          current_snapshot
              ? image_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_CRC,
                    L"PE image-create persistent Source state再証明",
                    L"read-only Sourceのpartition snapshotが初回状態から変化しました")
              : current_snapshot.error());
    }
    clonecore::Result<DirectImageCreateResumeStorageProof> current_storage =
        [&]() {
          try {
            return dependencies.prove_storage_separation(
                effective, runtime_record);
          } catch (...) {
            return failure<DirectImageCreateResumeStorageProof>(
                clonecore::ErrorCode::internal_error,
                ERROR_UNHANDLED_EXCEPTION,
                L"PE image-create storage reproof callback",
                L"opened-storage再証明callbackが例外で停止しました");
          }
        }();
    if (!current_storage ||
        !same_create_storage_proof(
            initial_storage, current_storage.value())) {
      return clonecore::Status::failure(
          current_storage
              ? image_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"PE image-create storage reproof",
                    L"checkpoint、Source、保存先storage domainが初回証明と一致しません")
              : current_storage.error());
    }
    return clonecore::success_status();
  };

  const auto checkpoint_matches_progress = [](
      const operationcore::ResumeSlotRecord& record,
      const imageformat::TsumugiCreateResumeProgressV1& progress) {
    auto parsed = resume_progress_from_checkpoint(
        record.checkpoint.checkpoint);
    return parsed && parsed.value() == progress;
  };

  imageformat::TsumugiCreateResumeCheckpointHooksV1 checkpoint_hooks{
      .create_before_first_mutation =
          [&](const imageformat::TsumugiCreateResumeOwnedPathsV1& paths,
              const imageformat::TsumugiCreateResumeBindingV1& binding,
              const imageformat::TsumugiCreateResumeProgressV1& progress) {
            if (!start_new || runtime_record || runtime_binding ||
                binding != stream_binding ||
                progress.phase != imageformat::
                    TsumugiCreateResumePhaseV1::preparing ||
                progress.verified_logical_bytes != 0U ||
                progress.verified_chunk_count != 0U) {
              return clonecore::Status::failure(image_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_INVALID_STATE,
                  L"PE image-create first durable slot",
                  L"新規preparing stateと完全stream bindingが必要です"));
            }
            const auto reproved = reprove_source_and_storage();
            if (!reproved) {
              return reproved;
            }
            auto image_object =
                operationcore::bind_windows_resume_owned_object(
                    paths.image_partial_path,
                    operationcore::ResumeOwnedObjectRole::image_partial,
                    operation_plan.operation_id,
                    identities);
            auto journal_object =
                operationcore::bind_windows_resume_owned_object(
                    paths.journal_path,
                    operationcore::ResumeOwnedObjectRole::
                        image_resume_journal,
                    operation_plan.operation_id,
                    identities);
            if (!image_object || !journal_object) {
              return clonecore::Status::failure(
                  image_object ? journal_object.error()
                               : image_object.error());
            }
            std::vector<operationcore::WindowsResumeOwnedObject> objects{
                image_object.value(), journal_object.value()};
            clonecore::Result<std::unique_ptr<
                operationcore::IResumeSlotPlatform>> made = [&]() {
              try {
                return dependencies.make_bound_slot_platform(objects);
              } catch (...) {
                return failure<std::unique_ptr<
                    operationcore::IResumeSlotPlatform>>(
                    clonecore::ErrorCode::internal_error,
                    ERROR_UNHANDLED_EXCEPTION,
                    L"PE image-create bound slot factory",
                    L"owned-object platform factoryが例外で停止しました");
              }
            }();
            if (!made) {
              return clonecore::Status::failure(made.error());
            }
            owned_platform = made.take_value();
            runtime_platform = owned_platform.get();
            auto checkpoint = parse_own_checkpoint(
                make_image_create_checkpoint(
                    operation_plan,
                    plan_hash.value(),
                    identities,
                    continuity_token,
                    progress,
                    1U));
            if (!checkpoint) {
              return clonecore::Status::failure(checkpoint.error());
            }
            operationcore::ResumeSlotRecord record{
                .capability = operationcore::ResumeCapability::
                    persistent_pe_exact_image_create,
                .checkpoint = checkpoint.value(),
                .identities = identities,
                .owned_partial = std::nullopt,
                .owned_objects = {
                    image_object.value().binding,
                    journal_object.value().binding,
                },
            };
            operationcore::SingleResumeSlot slot(*runtime_platform);
            const auto created = slot.create(record);
            if (!created) {
              return created;
            }
            auto inspected = slot.inspect();
            if (!inspected || !inspected.value()) {
              return clonecore::Status::failure(
                  inspected
                      ? image_error(
                            clonecore::ErrorCode::verification_failed,
                            ERROR_CRC,
                            L"PE image-create slot create readback",
                            L"durable create後のslotを確認できません")
                      : inspected.error());
            }
            auto binding_result = operationcore::make_resume_slot_binding(
                inspected.value().value());
            if (!binding_result) {
              return clonecore::Status::failure(binding_result.error());
            }
            runtime_record = std::move(inspected.value().value());
            runtime_binding = binding_result.take_value();
            return clonecore::success_status();
          },
      .prove_existing_before_resume =
          [&](const imageformat::TsumugiCreateResumeOwnedPathsV1&,
              const imageformat::TsumugiCreateResumeBindingV1& binding,
              const imageformat::TsumugiCreateResumeProgressV1& progress) {
            if (binding != stream_binding || !runtime_record ||
                !runtime_binding ||
                !checkpoint_matches_progress(*runtime_record, progress)) {
              return clonecore::Status::failure(image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"PE image-create existing prefix binding",
                  L"stream bindingまたはcheckpoint progressが一致しません"));
            }
            const auto reproved = reprove_source_and_storage();
            if (!reproved) {
              return reproved;
            }
            operationcore::SingleResumeSlot slot(*runtime_platform);
            auto bound = slot.open_bound(*runtime_binding);
            if (!bound ||
                !checkpoint_matches_progress(bound.value(), progress)) {
              return clonecore::Status::failure(
                  bound
                      ? image_error(
                            clonecore::ErrorCode::identity_mismatch,
                            ERROR_FILE_INVALID,
                            L"PE image-create opened prefix binding",
                            L"fresh exact-openしたslot progressが一致しません")
                      : bound.error());
            }
            runtime_record = bound.take_value();
            return clonecore::success_status();
          },
      .replace_after_verified_prefix =
          [&](const imageformat::TsumugiCreateResumeProgressV1& previous,
              const imageformat::TsumugiCreateResumeProgressV1& next) {
            if (!runtime_record || !runtime_binding ||
                !checkpoint_matches_progress(*runtime_record, previous) ||
                runtime_record->checkpoint.checkpoint.revision ==
                    (std::numeric_limits<std::uint64_t>::max)()) {
              return clonecore::Status::failure(image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"PE image-create checkpoint replace binding",
                  L"現在revisionまたはprevious progressを完全拘束できません"));
            }
            auto checkpoint = parse_own_checkpoint(
                make_image_create_checkpoint(
                    operation_plan,
                    plan_hash.value(),
                    identities,
                    continuity_token,
                    next,
                    runtime_record->checkpoint.checkpoint.revision + 1U));
            if (!checkpoint) {
              return clonecore::Status::failure(checkpoint.error());
            }
            operationcore::SingleResumeSlot slot(*runtime_platform);
            const auto replaced = slot.replace(
                *runtime_binding, checkpoint.value());
            if (!replaced) {
              return replaced;
            }
            auto inspected = slot.inspect();
            if (!inspected || !inspected.value()) {
              return clonecore::Status::failure(
                  inspected
                      ? image_error(
                            clonecore::ErrorCode::verification_failed,
                            ERROR_CRC,
                            L"PE image-create checkpoint replace readback",
                            L"replace後のslotを確認できません")
                      : inspected.error());
            }
            auto binding_result = operationcore::make_resume_slot_binding(
                inspected.value().value());
            if (!binding_result ||
                !checkpoint_matches_progress(
                    inspected.value().value(), next)) {
              return clonecore::Status::failure(
                  binding_result
                      ? image_error(
                            clonecore::ErrorCode::verification_failed,
                            ERROR_CRC,
                            L"PE image-create checkpoint progress readback",
                            L"replace後のprogressが次状態と一致しません")
                      : binding_result.error());
            }
            runtime_record = std::move(inspected.value().value());
            runtime_binding = binding_result.take_value();
            return clonecore::success_status();
          },
  };

  std::optional<imageformat::TsumugiCreateResumePreparedV1>
      prepared_output;
  if (!already_published) {
    auto prepared = imageformat::prepare_resumable_tsumugi_file_v1(
        imageformat::TsumugiCreateResumeRequestV1{
            .stream = std::move(stream_request),
            .binding = stream_binding,
            .expected_progress = expected_progress,
            .checkpoint = std::move(checkpoint_hooks),
        },
        effective.callbacks);
    if (!prepared) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          prepared.error());
    }
    if (!prepared.value().commit_ready ||
        !prepared.value().complete_partial_verified || !runtime_record ||
        !runtime_binding ||
        prepared.value().progress.phase != imageformat::
            TsumugiCreateResumePhaseV1::commit_ready ||
        !checkpoint_matches_progress(
            *runtime_record, prepared.value().progress)) {
      return failure<DirectImageCreateReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"PE image-create commit-ready evidence",
          L"完全検証済みpartialとdurable commit-ready checkpointが一致しません");
    }
    prepared_output = prepared.take_value();
  } else if (!expected_progress ||
             expected_progress->phase != imageformat::
                 TsumugiCreateResumePhaseV1::commit_ready ||
             !runtime_record || !runtime_binding) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_STATE,
        L"PE image-create post-publish recovery",
        L"公開後回復にはcommit-ready checkpointの完全bindingが必要です");
  }

  const auto final_snapshot = imageformat::capture_partition_snapshot_v1(
      session, table_style);
  if (!final_snapshot ||
      final_snapshot.value() != partition_snapshot.value()) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        final_snapshot
            ? image_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_CRC,
                  L"PE image-create publish前Source state",
                  L"Sourceのpartition snapshotが初回状態から変化しました")
            : final_snapshot.error());
  }
  const std::uint64_t expected_image_length =
      runtime_record->checkpoint.checkpoint.output_progress_evidence
          ->primary_output_length;
  if (!already_published) {
    destination_guard.phase = imageformat::
        WindowsTsumugiDestinationGuardPhase::before_commit_owned_partial;
    destination_guard.expected_owned_partial_bytes = expected_image_length;
    const auto destination =
        dependencies.direct.validate_destination(destination_guard);
    if (!destination) {
      return clonecore::Result<DirectImageCreateReport>::failure(
          destination.error());
    }
  }
  const auto before_publish = reprove_source_and_storage();
  if (!before_publish) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        before_publish.error());
  }

  std::optional<imageformat::TsumugiVerifiedImage> verified_final;
  auto committed = runtime_platform
      ->commit_persistent_pe_exact_image_create({
          .reviewed_binding = *runtime_binding,
          .reviewed_final_path = effective.final_path,
          .reprove_before_publish = reprove_source_and_storage,
          .verify_published_image =
              [&](const std::wstring& final_path) -> clonecore::Result<
                  operationcore::PersistentPeExactImageCreateVerification> {
                auto verified = imageformat::verify_tsumugi_image_v1(
                    imageformat::TsumugiImageVerifyRequest{
                        .image_path = final_path,
                        .storage_file_system = storage_file_system.value(),
                        .password = effective.encryption_password,
                        .verification_block_bytes =
                            4U * 1024U * 1024U,
                    },
                    effective.callbacks);
                if (!verified) {
                  return clonecore::Result<operationcore::
                      PersistentPeExactImageCreateVerification>::failure(
                      verified.error());
                }
                auto verified_manifest =
                    imageformat::build_tsumugi_manifest_v1(
                        verified.value().manifest);
                const auto& container = verified.value().container;
                if (!verified_manifest ||
                    verified_manifest.value() != manifest_bytes.value() ||
                    container.header.image_id != continuity.image_id ||
                    verified.value().manifest.source_state_hash !=
                        hashes.value().state ||
                    verified.value().partial_loss ||
                    !verified.value().unreadable_ranges.empty() ||
                    container.records.size() !=
                        prepared_plan.value().chunks.size()) {
                  return failure<operationcore::
                      PersistentPeExactImageCreateVerification>(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_CRC,
                      L"PE image-create published manifest binding",
                      L"完成fileのmanifest、image ID、Source stateまたはchunk planが一致しません");
                }
                operationcore::PersistentPeExactImageCreateVerification proof{
                    .image_length = container.opened_file.size,
                    .global_hash = container.global_hash,
                    .header_hash_verified = container.header_hash_verified,
                    .metadata_authenticated =
                        container.metadata_authenticated,
                    .all_chunks_verified = container.all_chunks_verified,
                    .global_hash_verified = container.global_hash_verified,
                };
                verified_final = verified.take_value();
                return clonecore::Result<operationcore::
                    PersistentPeExactImageCreateVerification>::success(
                    proof);
              },
      });
  if (!committed || !committed.value().image_published ||
      !committed.value().complete_image_verified ||
      !committed.value().journal_retired ||
      !committed.value().slot_retired || !verified_final) {
    return committed
        ? failure<DirectImageCreateReport>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"PE image-create publish/retire evidence",
              L"完成file完全検証、journal retire、またはslot retireが完了していません")
        : clonecore::Result<DirectImageCreateReport>::failure(
              committed.error());
  }

  const auto password_assessment = effective.encryption_password
      ? imageformat::assess_tsumugi_password(
            *effective.encryption_password)
      : imageformat::TsumugiPasswordAssessment{};
  const auto& container = verified_final->container;
  imageformat::TsumugiImageCreateReport image_report{
      .stream = {
          .final_path = effective.final_path,
          .retained_recovery_path = L"",
          .image_length = container.opened_file.size,
          .stored_data_bytes = container.header.data.length,
          .zero_filled_bytes = 0U,
          .chunk_count = container.records.size(),
          .replaced_existing = false,
          .all_chunks_read_back_verified = true,
          .all_chunks_authenticated_and_hashed = true,
          .global_hash_read_back_verified = true,
          .final_metadata_read_back_verified = true,
          .final_complete_scan_performed = true,
          .verification_mode = continuity.verification_mode,
          .committed = true,
      },
      .unreadable_ranges = {},
      .encrypted = continuity.encrypted,
      .password_was_weak = password_assessment.weak,
      .selected_verification_passed = true,
      .complete_verification_passed = true,
  };
  if (!imageformat::selected_tsumugi_creation_verification_passed(
          image_report)) {
    return failure<DirectImageCreateReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE image-create selected verification evidence",
        L"選択方式以上の完全検証証跡を中央gateが受理しませんでした");
  }
  return clonecore::Result<DirectImageCreateReport>::success({
      .source_identity = source.value().observed.identity,
      .source_partition_style =
          source.value().observed.observed.partition_style,
      .imaged_partition_count =
          prepared_plan.value().selected_partition_count,
      .logical_payload_bytes = prepared_plan.value().logical_payload_bytes,
      .source_read_only_verified = true,
      .source_left_read_only = true,
      .layout_revalidated_before_commit = true,
      .rescue_mode = false,
      .rescue = std::nullopt,
      .image = std::move(image_report),
  });
}

clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_with_windows_apis(
    const DirectImageCreateRequest& request) {
  return execute_direct_image_create(
      request,
      DirectImageCreateDependencies{
          .set_source_read_only =
              [](const clonecore::StableDiskIdentity& source,
                 const bool read_only) {
                return diskmodel::
                    set_verified_source_read_only_with_windows_apis(
                        source, read_only);
              },
          .open_read_only_source =
              [](const clonecore::StableDiskIdentity& source) {
                return diskmodel::
                    open_verified_read_only_physical_disk_with_windows_apis(
                        source);
              },
          .query_destination_file_system =
              query_destination_file_system_with_windows_apis,
          .validate_destination =
              [](const imageformat::WindowsTsumugiDestinationGuardRequest&
                     guard) {
                return imageformat::validate_windows_tsumugi_destination(
                    guard);
              },
          .make_rescue_staging =
              imageformat::make_windows_tsumugi_rescue_staging_session,
      });
}

clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_resume_with_windows_apis_v1(
    const DirectImageCreateRequest& request,
    const DirectImageCreateResumeCommand& command) {
  auto storage =
      make_direct_image_create_windows_resume_storage_platform_v1();
  if (!storage) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        storage.error());
  }
  auto platform = operationcore::
      make_current_executable_windows_resume_slot_platform(
          storage.value().prove_data_backing);
  if (!platform) {
    return clonecore::Result<DirectImageCreateReport>::failure(
        platform.error());
  }
  return execute_direct_image_create_resume_v1(
      request,
      command,
      DirectImageCreateResumeDependencies{
          .direct = {
              .set_source_read_only =
                  [](const clonecore::StableDiskIdentity& source,
                     const bool read_only) {
                    return diskmodel::
                        set_verified_source_read_only_with_windows_apis(
                            source, read_only);
                  },
              .open_read_only_source =
                  [](const clonecore::StableDiskIdentity& source) {
                    return diskmodel::
                        open_verified_read_only_physical_disk_with_windows_apis(
                            source);
                  },
              .query_destination_file_system =
                  query_destination_file_system_with_windows_apis,
              .validate_destination =
                  [](const imageformat::
                         WindowsTsumugiDestinationGuardRequest& guard) {
                    return imageformat::validate_windows_tsumugi_destination(
                        guard);
                  },
              .make_rescue_staging =
                  imageformat::make_windows_tsumugi_rescue_staging_session,
          },
          .slot_platform = platform.value().get(),
          .make_bound_slot_platform =
              storage.value().make_bound_slot_platform,
          .prove_storage_separation =
              storage.value().prove_image_create_storage,
      });
}

}  // namespace ytec::winpeapp

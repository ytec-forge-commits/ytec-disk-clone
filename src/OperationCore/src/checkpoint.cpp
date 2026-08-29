#include "ytec/operationcore/checkpoint.h"

#include "sha256_internal.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>

namespace ytec::operationcore {
namespace {

constexpr std::uint16_t kCheckpointMajor = 1U;
constexpr std::uint16_t kCheckpointMinorV1 = 0U;
constexpr std::uint16_t kCheckpointMinorV2 = 1U;
constexpr std::uint16_t kCheckpointMinorV3 = 2U;
constexpr std::size_t kDigestBytes = Sha256Digest{}.size();
constexpr std::size_t kPreparationSectorWireBytes =
    sizeof(std::uint64_t) + sizeof(std::uint64_t) + kDigestBytes;
constexpr std::size_t kMaximumContinuityCharacters = 512U;
constexpr std::size_t kMaximumModelCharacters = 256U;
constexpr std::size_t kMaximumSerialCharacters = 128U;
constexpr std::size_t kMaximumDeviceIdCharacters = 1024U;
constexpr std::array<std::byte, 8> kCheckpointMagic{
    std::byte{0x59}, std::byte{0x54}, std::byte{0x45}, std::byte{0x43},
    std::byte{0x43}, std::byte{0x50}, std::byte{0x31}, std::byte{0x00}};

clonecore::Error checkpoint_error(
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

clonecore::Status checkpoint_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(checkpoint_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool operation_id_equal(
    const OperationId& left,
    const OperationId& right) noexcept {
  unsigned int difference = 0U;
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

bool identity_equal(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  // Disk numbers are intentionally excluded: they are routing hints and may
  // change after reconnect or PE boot. Every stable signal must still match.
  return left.model == right.model &&
         left.size_bytes == right.size_bytes &&
         left.logical_sector_size == right.logical_sector_size &&
         left.serial_suffix == right.serial_suffix &&
         left.device_instance_id == right.device_instance_id &&
         left.is_system_disk == right.is_system_disk;
}

bool optional_identity_equal(
    const std::optional<clonecore::StableDiskIdentity>& left,
    const std::optional<clonecore::StableDiskIdentity>& right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left || identity_equal(*left, *right);
}

bool checkpoint_phase_known(const CheckpointPhase phase) noexcept {
  switch (phase) {
    case CheckpointPhase::executing:
    case CheckpointPhase::verifying:
    case CheckpointPhase::preparing:
    case CheckpointPhase::prepared:
    case CheckpointPhase::commit_ready:
      return true;
  }
  return false;
}

bool legacy_checkpoint_phase(const CheckpointPhase phase) noexcept {
  return phase == CheckpointPhase::executing ||
      phase == CheckpointPhase::verifying;
}

bool preparation_checkpoint_phase(const CheckpointPhase phase) noexcept {
  return phase == CheckpointPhase::preparing ||
      phase == CheckpointPhase::prepared ||
      phase == CheckpointPhase::commit_ready;
}

bool valid_continuity_token(const std::wstring& value) {
  if (value.empty() || value.size() > kMaximumContinuityCharacters) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](const wchar_t character) {
    return character == L'\0' || character < L' ' || character == 0x7F;
  });
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xFFU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
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

void set_u32(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes[offset + (shift / 8U)] =
        static_cast<std::byte>((value >> shift) & 0xFFU);
  }
}

void append_wstring(std::vector<std::byte>& bytes, const std::wstring& value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    const auto code_unit = static_cast<std::uint16_t>(character);
    append_u16(bytes, code_unit);
  }
}

void append_string(std::vector<std::byte>& bytes, const std::string& value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const unsigned char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
}

void append_identity(
    std::vector<std::byte>& bytes,
    const clonecore::StableDiskIdentity& identity) {
  append_u32(bytes, identity.disk_number);
  append_u64(bytes, identity.size_bytes);
  append_u32(bytes, identity.logical_sector_size);
  append_u8(bytes, identity.is_system_disk ? 1U : 0U);
  append_wstring(bytes, identity.model);
  append_string(bytes, identity.serial_suffix);
  append_wstring(bytes, identity.device_instance_id);
}

class Reader final {
 public:
  explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool read_u8(std::uint8_t& value) {
    if (!has(1U)) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[offset_]);
    ++offset_;
    return true;
  }

  [[nodiscard]] bool read_u16(std::uint16_t& value) {
    if (!has(2U)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes_[offset_]) |
        (std::to_integer<std::uint16_t>(bytes_[offset_ + 1U]) << 8U));
    offset_ += 2U;
    return true;
  }

  [[nodiscard]] bool read_u32(std::uint32_t& value) {
    if (!has(4U)) {
      return false;
    }
    value = 0U;
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
      value |= std::to_integer<std::uint32_t>(
                   bytes_[offset_ + (shift / 8U)])
               << shift;
    }
    offset_ += 4U;
    return true;
  }

  [[nodiscard]] bool read_u64(std::uint64_t& value) {
    if (!has(8U)) {
      return false;
    }
    value = 0U;
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
      value |= std::to_integer<std::uint64_t>(
                   bytes_[offset_ + (shift / 8U)])
               << shift;
    }
    offset_ += 8U;
    return true;
  }

  template <std::size_t Size>
  [[nodiscard]] bool read_array(std::array<std::byte, Size>& value) {
    if (!has(Size)) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), Size,
                value.begin());
    offset_ += Size;
    return true;
  }

  [[nodiscard]] bool read_wstring(
      std::wstring& value,
      const std::size_t maximum_characters) {
    std::uint32_t count{};
    if (!read_u32(count) || count > maximum_characters ||
        static_cast<std::size_t>(count) >
            (std::numeric_limits<std::size_t>::max() / 2U) ||
        !has(static_cast<std::size_t>(count) * 2U)) {
      return false;
    }
    value.clear();
    value.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
      std::uint16_t code_unit{};
      if (!read_u16(code_unit) || code_unit == 0U) {
        return false;
      }
      value.push_back(static_cast<wchar_t>(code_unit));
    }
    return true;
  }

  [[nodiscard]] bool read_string(
      std::string& value,
      const std::size_t maximum_characters) {
    std::uint32_t count{};
    if (!read_u32(count) || count > maximum_characters || !has(count)) {
      return false;
    }
    value.clear();
    value.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
      const auto character =
          std::to_integer<unsigned char>(bytes_[offset_ + index]);
      if (character == 0U || character < 0x20U || character == 0x7FU) {
        return false;
      }
      value.push_back(static_cast<char>(character));
    }
    offset_ += count;
    return true;
  }

  [[nodiscard]] bool at_end() const noexcept {
    return offset_ == bytes_.size();
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - offset_;
  }

 private:
  [[nodiscard]] bool has(const std::size_t amount) const noexcept {
    return amount <= bytes_.size() - offset_;
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

bool read_identity(
    Reader& reader,
    clonecore::StableDiskIdentity& identity) {
  std::uint8_t is_system{};
  if (!reader.read_u32(identity.disk_number) ||
      !reader.read_u64(identity.size_bytes) ||
      !reader.read_u32(identity.logical_sector_size) ||
      !reader.read_u8(is_system) || is_system > 1U ||
      !reader.read_wstring(identity.model, kMaximumModelCharacters) ||
      !reader.read_string(identity.serial_suffix, kMaximumSerialCharacters) ||
      !reader.read_wstring(
          identity.device_instance_id, kMaximumDeviceIdCharacters)) {
    return false;
  }
  identity.is_system_disk = is_system != 0U;
  return true;
}

clonecore::Status validate_preparation_evidence(
    const InterruptionCheckpoint& checkpoint) {
  const bool schema_v1 =
      checkpoint.schema_version == kCheckpointSchemaVersionV1;
  const bool schema_v2 =
      checkpoint.schema_version == kCheckpointSchemaVersionV2;
  const bool schema_v3 =
      checkpoint.schema_version == kCheckpointSchemaVersionV3;
  const bool legacy_phase = legacy_checkpoint_phase(checkpoint.phase);
  const bool preparation_phase =
      preparation_checkpoint_phase(checkpoint.phase);
  if ((!schema_v1 && !schema_v2 && !schema_v3) ||
      (schema_v1 && (!legacy_phase || checkpoint.preparation_evidence)) ||
      (schema_v2 &&
       (preparation_phase != checkpoint.preparation_evidence.has_value())) ||
      (schema_v3 && checkpoint.preparation_evidence)) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_REVISION_MISMATCH,
        L"中断チェックポイント準備証跡版",
        L"schema、phase、または準備証跡の組合せが対応範囲外です");
  }
  if (!checkpoint.preparation_evidence) {
    return clonecore::success_status();
  }
  if (!checkpoint.target) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント準備対象",
        L"準備証跡には安定再識別可能なtargetが必要です");
  }

  const auto& evidence = *checkpoint.preparation_evidence;
  if (detail::digest_is_zero(evidence.initial_layout_hash) ||
      evidence.logical_sector_size == 0U ||
      evidence.logical_sector_size != checkpoint.target->logical_sector_size ||
      evidence.original_sectors.empty() ||
      evidence.original_sectors.size() >
          kMaximumCheckpointPreparationSectors) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント準備証跡",
        L"初期layout Hash、sector寸法、target、またはsector件数が不正です");
  }

  std::uint64_t previous_end{};
  bool first = true;
  for (const auto& sector : evidence.original_sectors) {
    if (sector.length != evidence.logical_sector_size ||
        sector.offset % evidence.logical_sector_size != 0U ||
        sector.offset > checkpoint.target->size_bytes ||
        sector.length > checkpoint.target->size_bytes - sector.offset ||
        (!first && sector.offset < previous_end) ||
        detail::digest_is_zero(sector.original_hash)) {
      return checkpoint_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"中断チェックポイント準備sector証跡",
          L"sectorが未整列、重複、範囲外、sector寸法不一致、またはzero Hashです");
    }
    previous_end = sector.offset + sector.length;
    first = false;
  }
  return clonecore::success_status();
}

clonecore::Status validate_output_progress_evidence(
    const InterruptionCheckpoint& checkpoint) {
  const bool schema_v3 =
      checkpoint.schema_version == kCheckpointSchemaVersionV3;
  if (schema_v3 != checkpoint.output_progress_evidence.has_value()) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_REVISION_MISMATCH,
        L"中断チェックポイント出力進捗証跡版",
        L"schema v3だけが出力進捗証跡を必須とします");
  }
  if (!schema_v3) {
    return clonecore::success_status();
  }
  const auto& evidence = *checkpoint.output_progress_evidence;
  if (checkpoint.kind != OperationKind::image_create ||
      checkpoint.environment != OperationEnvironment::winpe ||
      !preparation_checkpoint_phase(checkpoint.phase) ||
      detail::digest_is_zero(evidence.verified_prefix_hash) ||
      evidence.auxiliary_output_length != 0U ||
      (checkpoint.phase != CheckpointPhase::preparing &&
       evidence.journal_length == 0U) ||
      (checkpoint.verified_work_bytes != 0U &&
       evidence.primary_output_length == 0U)) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント出力進捗証跡",
        L"WinPE exact image-createの段階、prefix Hash、または所有オブジェク長が不正です");
  }
  return clonecore::success_status();
}

clonecore::Status validate_path(const std::wstring& path) {
  constexpr std::wstring_view kExtension = L".checkpoint";
  if (path.size() < 16U || path.size() >= 32U * 1024U ||
      std::iswalpha(static_cast<wint_t>(path[0])) == 0 ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'/') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      path.find(L"\\..\\") != std::wstring::npos ||
      path.ends_with(L"\\..") || path.ends_with(L"\\") ||
      path.ends_with(L" ") || path.ends_with(L".") ||
      path.size() <= kExtension.size()) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"中断チェックポイントパス",
        L"ローカルドライブ上の正規化した.checkpointパスが必要です");
  }
  const std::wstring_view suffix =
      std::wstring_view(path).substr(path.size() - kExtension.size());
  if (CompareStringOrdinal(
          suffix.data(),
          static_cast<int>(suffix.size()),
          kExtension.data(),
          static_cast<int>(kExtension.size()),
          TRUE) != CSTR_EQUAL) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"中断チェックポイントパス",
        L"保存先拡張子は.checkpointでなければなりません");
  }

  std::size_t separator = path.find(L'\\', 3U);
  while (separator != std::wstring::npos) {
    const std::wstring parent = path.substr(0U, separator);
    const DWORD attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"中断チェックポイント親要素",
          GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return checkpoint_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          L"中断チェックポイント親要素",
          L"親要素が通常ディレクトリでないかreparse pointです");
    }
    separator = path.find(L'\\', separator + 1U);
  }
  return clonecore::success_status();
}

clonecore::Result<ParsedCheckpoint> read_checkpoint_handle(
    const HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information)) {
    return clonecore::Result<ParsedCheckpoint>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"中断チェックポイント属性確認",
            GetLastError()));
  }
  if ((information.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"中断チェックポイント属性確認",
        L"通常ファイル以外は読み込めません"));
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle, &size)) {
    return clonecore::Result<ParsedCheckpoint>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"中断チェックポイント寸法確認",
            GetLastError()));
  }
  if (size.QuadPart <= 0 ||
      static_cast<unsigned long long>(size.QuadPart) >
          kMaximumCheckpointBytes) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"中断チェックポイント寸法確認",
        L"チェックポイント寸法が安全上限外です"));
  }

  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
    return clonecore::Result<ParsedCheckpoint>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"中断チェックポイント読取り位置",
            GetLastError()));
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD read{};
  if (!ReadFile(
          handle,
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &read,
          nullptr) ||
      static_cast<std::size_t>(read) != bytes.size()) {
    return clonecore::Result<ParsedCheckpoint>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"中断チェックポイント読取り",
            GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF
                                            : GetLastError()));
  }
  return parse_checkpoint(bytes);
}

clonecore::Result<std::optional<ParsedCheckpoint>> read_existing(
    const std::wstring& path,
    const DWORD desired_access,
    clonecore::UniqueHandle& opened) {
  opened.reset(CreateFileW(
      path.c_str(),
      desired_access,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!opened) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<std::optional<ParsedCheckpoint>>::success(
          std::nullopt);
    }
    return clonecore::Result<std::optional<ParsedCheckpoint>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"中断チェックポイントを開く",
            native_code));
  }
  auto parsed = read_checkpoint_handle(opened.get());
  if (!parsed) {
    return clonecore::Result<std::optional<ParsedCheckpoint>>::failure(
        parsed.error());
  }
  return clonecore::Result<std::optional<ParsedCheckpoint>>::success(
      std::optional<ParsedCheckpoint>(parsed.take_value()));
}

void mark_owned_file_for_deletion(const HANDLE handle) noexcept {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  static_cast<void>(SetFileInformationByHandle(
      handle,
      FileDispositionInfo,
      &disposition,
      sizeof(disposition)));
}

clonecore::Status write_verified_stage(
    const std::wstring& stage_path,
    const std::span<const std::byte> bytes,
    const Sha256Digest& expected_hash) {
  clonecore::UniqueHandle file(CreateFileW(
      stage_path.c_str(),
      GENERIC_READ | GENERIC_WRITE | DELETE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!file) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"中断チェックポイント一時ファイル新規作成",
        GetLastError()));
  }

  const auto fail_owned = [&file](clonecore::Status status) {
    mark_owned_file_for_deletion(file.get());
    file.reset();
    return status;
  };

  DWORD written{};
  if (!WriteFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &written,
          nullptr) ||
      static_cast<std::size_t>(written) != bytes.size()) {
    const DWORD native_code = GetLastError();
    return fail_owned(clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"中断チェックポイント一時ファイル書込み",
        native_code == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native_code)));
  }
  if (!FlushFileBuffers(file.get())) {
    return fail_owned(clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"中断チェックポイント一時ファイルflush",
        GetLastError())));
  }

  auto parsed = read_checkpoint_handle(file.get());
  if (!parsed ||
      !detail::digest_equal(parsed.value().record_hash, expected_hash)) {
    return fail_owned(checkpoint_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"中断チェックポイント一時ファイル読戻し",
        L"保存内容の版、寸法、識別情報、またはHashが一致しません"));
  }
  file.reset();
  return clonecore::success_status();
}

void discard_owned_stage_if_matching(
    const std::wstring& stage_path,
    const Sha256Digest& expected_hash) noexcept {
  clonecore::UniqueHandle file(CreateFileW(
      stage_path.c_str(),
      GENERIC_READ | DELETE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    return;
  }
  const auto parsed = read_checkpoint_handle(file.get());
  if (!parsed ||
      !detail::digest_equal(parsed.value().record_hash, expected_hash)) {
    return;
  }
  mark_owned_file_for_deletion(file.get());
}

clonecore::Status verify_committed_file(
    const std::wstring& path,
    const Sha256Digest& expected_hash) {
  clonecore::UniqueHandle file;
  auto parsed = read_existing(path, GENERIC_READ, file);
  if (!parsed || !parsed.value() ||
      !detail::digest_equal(
          parsed.value()->record_hash, expected_hash)) {
    return checkpoint_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"中断チェックポイント確定後検証",
        L"確定後の版、寸法、識別情報、またはHashが一致しません");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Status validate_checkpoint(
    const InterruptionCheckpoint& checkpoint) {
  if ((checkpoint.schema_version != kCheckpointSchemaVersionV1 &&
       checkpoint.schema_version != kCheckpointSchemaVersionV2 &&
       checkpoint.schema_version != kCheckpointSchemaVersionV3) ||
      !checkpoint_phase_known(checkpoint.phase) || checkpoint.revision == 0U ||
      checkpoint.expected_work_bytes == 0U ||
      checkpoint.verified_work_bytes > checkpoint.expected_work_bytes ||
      (checkpoint.verified_work_bytes == 0U &&
       checkpoint.verified_chunk_count != 0U) ||
      (checkpoint.verified_work_bytes != 0U &&
       checkpoint.verified_chunk_count == 0U) ||
      (checkpoint.phase == CheckpointPhase::preparing &&
       (checkpoint.verified_work_bytes != 0U ||
        checkpoint.verified_chunk_count != 0U)) ||
      (checkpoint.phase == CheckpointPhase::commit_ready &&
       (checkpoint.verified_work_bytes != checkpoint.expected_work_bytes ||
        checkpoint.verified_chunk_count == 0U)) ||
      detail::digest_is_zero(checkpoint.plan_hash) ||
      detail::digest_is_zero(checkpoint.output_identity_hash) ||
      !valid_continuity_token(checkpoint.continuity_token)) {
    return checkpoint_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント",
        L"版、進捗、継続性トークン、またはHashが不正です");
  }

  const auto preparation_valid = validate_preparation_evidence(checkpoint);
  if (!preparation_valid) {
    return preparation_valid;
  }
  const auto output_progress_valid =
      validate_output_progress_evidence(checkpoint);
  if (!output_progress_valid) {
    return output_progress_valid;
  }

  OperationPlan shape{
      .schema_version = kOperationPlanSchemaVersion,
      .operation_id = checkpoint.operation_id,
      .kind = checkpoint.kind,
      .environment = checkpoint.environment,
      .source = checkpoint.source,
      .target = checkpoint.target,
      .expected_work_bytes = checkpoint.expected_work_bytes,
      .immutable_payload_hash = checkpoint.plan_hash,
  };
  return validate_operation_plan(shape);
}

clonecore::Status validate_checkpoint_transition(
    const InterruptionCheckpoint& current,
    const InterruptionCheckpoint& next) {
  const auto current_valid = validate_checkpoint(current);
  if (!current_valid) {
    return current_valid;
  }
  const auto next_valid = validate_checkpoint(next);
  if (!next_valid) {
    return next_valid;
  }
  const bool same_progress =
      next.verified_work_bytes == current.verified_work_bytes &&
      next.verified_chunk_count == current.verified_chunk_count;
  const bool advanced_progress =
      next.verified_work_bytes > current.verified_work_bytes ||
      next.verified_chunk_count > current.verified_chunk_count;
  // A legacy executing record may be upgraded exactly once after the owning
  // controller has captured schema-v2 preparation evidence. A zero cursor can
  // enter preparing; an already-invalidated/payload-progress record can enter
  // prepared only after the caller's read-only target proof. No other schema
  // migration is accepted here.
  const bool legacy_schema_upgrade =
      current.schema_version == kCheckpointSchemaVersionV1 &&
      next.schema_version == kCheckpointSchemaVersionV2 &&
      current.phase == CheckpointPhase::executing && same_progress &&
      next.preparation_evidence.has_value() &&
      (next.phase == CheckpointPhase::prepared ||
       (next.phase == CheckpointPhase::preparing &&
        current.verified_work_bytes == 0U &&
        current.verified_chunk_count == 0U));
  const bool output_schema_v3 =
      current.schema_version == kCheckpointSchemaVersionV3 &&
      next.schema_version == kCheckpointSchemaVersionV3;
  bool allowed_phase_transition = false;
  switch (current.phase) {
    case CheckpointPhase::executing:
      allowed_phase_transition =
          (next.phase == CheckpointPhase::executing && advanced_progress) ||
          next.phase == CheckpointPhase::verifying || legacy_schema_upgrade;
      break;
    case CheckpointPhase::verifying:
      allowed_phase_transition =
          next.phase == CheckpointPhase::verifying && advanced_progress;
      break;
    case CheckpointPhase::preparing:
      allowed_phase_transition =
          next.phase == CheckpointPhase::prepared && same_progress;
      break;
    case CheckpointPhase::prepared:
      allowed_phase_transition =
          (next.phase == CheckpointPhase::prepared && advanced_progress) ||
          (next.phase == CheckpointPhase::commit_ready && same_progress &&
           current.verified_work_bytes == current.expected_work_bytes &&
           current.verified_chunk_count != 0U);
      break;
    case CheckpointPhase::commit_ready:
      allowed_phase_transition = false;
      break;
  }
  if (output_schema_v3) {
    const auto& current_output = *current.output_progress_evidence;
    const auto& next_output = *next.output_progress_evidence;
    const bool output_lengths_monotonic =
        next_output.primary_output_length >=
            current_output.primary_output_length &&
        next_output.journal_length >= current_output.journal_length &&
        next_output.auxiliary_output_length >=
            current_output.auxiliary_output_length;
    const bool output_evidence_changed =
        next_output != current_output;
    if (!output_lengths_monotonic ||
        (advanced_progress && !output_evidence_changed) ||
        (current.phase == CheckpointPhase::preparing &&
         next.phase == CheckpointPhase::prepared &&
         !output_evidence_changed) ||
        (current.phase == CheckpointPhase::prepared &&
         next.phase == CheckpointPhase::commit_ready &&
         !output_evidence_changed)) {
      allowed_phase_transition = false;
    }
  }
  if ((current.schema_version != next.schema_version &&
       !legacy_schema_upgrade) ||
      !operation_id_equal(current.operation_id, next.operation_id) ||
      current.kind != next.kind || current.environment != next.environment ||
      current.expected_work_bytes != next.expected_work_bytes ||
      !detail::digest_equal(current.plan_hash, next.plan_hash) ||
      !detail::digest_equal(
          current.output_identity_hash, next.output_identity_hash) ||
      !optional_identity_equal(current.source, next.source) ||
      !optional_identity_equal(current.target, next.target) ||
      current.continuity_token != next.continuity_token ||
      (current.preparation_evidence != next.preparation_evidence &&
       !legacy_schema_upgrade) ||
      (!output_schema_v3 &&
       current.output_progress_evidence != next.output_progress_evidence) ||
      current.revision == std::numeric_limits<std::uint64_t>::max() ||
      next.revision != current.revision + 1U ||
      next.verified_work_bytes < current.verified_work_bytes ||
      next.verified_chunk_count < current.verified_chunk_count ||
      !allowed_phase_transition) {
    return checkpoint_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"中断チェックポイント更新",
        L"操作識別、版、継続性、進捗、または段階が安全な単調更新ではありません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_checkpoint_for_resume(
    const InterruptionCheckpoint& checkpoint,
    const OperationPlan& plan,
    const ReidentifiedOperation& observed,
    const std::wstring_view continuity_token,
    const Sha256Digest& output_identity_hash) {
  const auto checkpoint_valid = validate_checkpoint(checkpoint);
  if (!checkpoint_valid) {
    return checkpoint_valid;
  }
  const auto identities = validate_reidentified_operation(plan, observed);
  if (!identities) {
    return identities;
  }
  const auto plan_hash = hash_operation_plan(plan);
  if (!plan_hash) {
    return clonecore::Status::failure(plan_hash.error());
  }

  if (!operation_id_equal(checkpoint.operation_id, plan.operation_id) ||
      checkpoint.kind != plan.kind ||
      checkpoint.environment != plan.environment ||
      checkpoint.expected_work_bytes != plan.expected_work_bytes ||
      !detail::digest_equal(checkpoint.plan_hash, plan_hash.value()) ||
      !optional_identity_equal(checkpoint.source, plan.source) ||
      !optional_identity_equal(checkpoint.target, plan.target) ||
      checkpoint.continuity_token != continuity_token ||
      !detail::digest_equal(
          checkpoint.output_identity_hash, output_identity_hash)) {
    return checkpoint_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"中断処理の再開検証",
        L"同じ操作計画、ディスク、Snapshot状態、または出力を証明できません");
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> serialize_checkpoint(
    const InterruptionCheckpoint& checkpoint) {
  const auto valid = validate_checkpoint(checkpoint);
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }

  std::vector<std::byte> bytes;
  const std::size_t preparation_bytes = checkpoint.preparation_evidence
      ? kDigestBytes + sizeof(std::uint32_t) + sizeof(std::uint32_t) +
          checkpoint.preparation_evidence->original_sectors.size() *
              kPreparationSectorWireBytes
      : 0U;
  constexpr std::size_t kOutputProgressWireBytes =
      kDigestBytes + (3U * sizeof(std::uint64_t));
  const std::size_t output_progress_bytes =
      checkpoint.output_progress_evidence ? kOutputProgressWireBytes : 0U;
  bytes.reserve(512U + preparation_bytes + output_progress_bytes);
  append_array(bytes, kCheckpointMagic);
  append_u16(bytes, kCheckpointMajor);
  append_u16(
      bytes,
      checkpoint.schema_version == kCheckpointSchemaVersionV1
          ? kCheckpointMinorV1
          : (checkpoint.schema_version == kCheckpointSchemaVersionV2
                 ? kCheckpointMinorV2
                 : kCheckpointMinorV3));
  constexpr std::size_t kTotalSizeOffset = 12U;
  append_u32(bytes, 0U);
  append_u8(bytes, static_cast<std::uint8_t>(checkpoint.kind));
  append_u8(bytes, static_cast<std::uint8_t>(checkpoint.environment));
  append_u8(bytes, static_cast<std::uint8_t>(checkpoint.phase));
  append_u8(
      bytes,
      static_cast<std::uint8_t>(
          (checkpoint.source ? 0x01U : 0U) |
          (checkpoint.target ? 0x02U : 0U) |
          (checkpoint.preparation_evidence ? 0x04U : 0U) |
          (checkpoint.output_progress_evidence ? 0x08U : 0U)));
  append_u32(bytes, 0U);
  append_u64(bytes, checkpoint.revision);
  append_u64(bytes, checkpoint.expected_work_bytes);
  append_u64(bytes, checkpoint.verified_work_bytes);
  append_u64(bytes, checkpoint.verified_chunk_count);
  append_array(bytes, checkpoint.operation_id);
  append_array(bytes, checkpoint.plan_hash);
  append_array(bytes, checkpoint.output_identity_hash);
  append_wstring(bytes, checkpoint.continuity_token);
  if (checkpoint.source) {
    append_identity(bytes, *checkpoint.source);
  }
  if (checkpoint.target) {
    append_identity(bytes, *checkpoint.target);
  }
  if (checkpoint.preparation_evidence) {
    append_array(
        bytes, checkpoint.preparation_evidence->initial_layout_hash);
    append_u32(
        bytes, checkpoint.preparation_evidence->logical_sector_size);
    append_u32(
        bytes,
        static_cast<std::uint32_t>(
            checkpoint.preparation_evidence->original_sectors.size()));
    for (const auto& sector :
         checkpoint.preparation_evidence->original_sectors) {
      append_u64(bytes, sector.offset);
      append_u64(bytes, sector.length);
      append_array(bytes, sector.original_hash);
    }
  }
  if (checkpoint.output_progress_evidence) {
    append_array(
        bytes, checkpoint.output_progress_evidence->verified_prefix_hash);
    append_u64(
        bytes, checkpoint.output_progress_evidence->primary_output_length);
    append_u64(bytes, checkpoint.output_progress_evidence->journal_length);
    append_u64(
        bytes, checkpoint.output_progress_evidence->auxiliary_output_length);
  }

  if (bytes.size() > kMaximumCheckpointBytes - kDigestBytes) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        checkpoint_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_FILE_TOO_LARGE,
            L"中断チェックポイント直列化",
            L"チェックポイントが安全上限を超えています"));
  }
  const auto total_size = static_cast<std::uint32_t>(
      bytes.size() + kDigestBytes);
  set_u32(bytes, kTotalSizeOffset, total_size);
  const auto digest = detail::sha256(bytes);
  if (!digest) {
    return clonecore::Result<std::vector<std::byte>>::failure(digest.error());
  }
  append_array(bytes, digest.value());
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Result<ParsedCheckpoint> parse_checkpoint(
    const std::span<const std::byte> bytes) {
  constexpr std::size_t kMinimumFixedBytes = 172U;
  if (bytes.size() < kMinimumFixedBytes ||
      bytes.size() > kMaximumCheckpointBytes) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント寸法",
        L"チェックポイント寸法が固定範囲外です"));
  }

  const std::span<const std::byte> payload =
      bytes.first(bytes.size() - kDigestBytes);
  Sha256Digest stored_hash{};
  std::copy_n(
      bytes.end() - static_cast<std::ptrdiff_t>(kDigestBytes),
      kDigestBytes,
      stored_hash.begin());

  Reader reader(payload);
  std::array<std::byte, 8> magic{};
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint32_t total_size{};
  if (!reader.read_array(magic) || magic != kCheckpointMagic ||
      !reader.read_u16(major) || !reader.read_u16(minor) ||
      !reader.read_u32(total_size) || major != kCheckpointMajor ||
      (minor != kCheckpointMinorV1 && minor != kCheckpointMinorV2 &&
       minor != kCheckpointMinorV3) ||
      total_size != bytes.size()) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_REVISION_MISMATCH,
        L"中断チェックポイント版と寸法",
        L"Magic、対応版、または宣言寸法が一致しません"));
  }

  const auto calculated_hash = detail::sha256(payload);
  if (!calculated_hash ||
      !detail::digest_equal(calculated_hash.value(), stored_hash)) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"中断チェックポイントHash",
        L"チェックポイントのSHA-256が一致しません"));
  }

  std::uint8_t kind{};
  std::uint8_t environment{};
  std::uint8_t phase{};
  std::uint8_t flags{};
  std::uint32_t reserved{};
  InterruptionCheckpoint checkpoint;
  const std::uint8_t allowed_flags = minor == kCheckpointMinorV1
      ? 0x03U
      : (minor == kCheckpointMinorV2 ? 0x07U : 0x0BU);
  if (!reader.read_u8(kind) || !reader.read_u8(environment) ||
      !reader.read_u8(phase) || !reader.read_u8(flags) ||
      (flags & ~allowed_flags) != 0U || !reader.read_u32(reserved) ||
      reserved != 0U || !reader.read_u64(checkpoint.revision) ||
      !reader.read_u64(checkpoint.expected_work_bytes) ||
      !reader.read_u64(checkpoint.verified_work_bytes) ||
      !reader.read_u64(checkpoint.verified_chunk_count) ||
      !reader.read_array(checkpoint.operation_id) ||
      !reader.read_array(checkpoint.plan_hash) ||
      !reader.read_array(checkpoint.output_identity_hash) ||
      !reader.read_wstring(
          checkpoint.continuity_token, kMaximumContinuityCharacters)) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント構造",
        L"固定フィールドまたは継続性トークンが不正です"));
  }
  checkpoint.schema_version = minor == kCheckpointMinorV1
      ? kCheckpointSchemaVersionV1
      : (minor == kCheckpointMinorV2
             ? kCheckpointSchemaVersionV2
             : kCheckpointSchemaVersionV3);
  checkpoint.kind = static_cast<OperationKind>(kind);
  checkpoint.environment = static_cast<OperationEnvironment>(environment);
  checkpoint.phase = static_cast<CheckpointPhase>(phase);
  if ((flags & 0x01U) != 0U) {
    clonecore::StableDiskIdentity source;
    if (!read_identity(reader, source)) {
      return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"中断チェックポイントのコピー元識別",
          L"コピー元識別情報が不正です"));
    }
    checkpoint.source = std::move(source);
  }
  if ((flags & 0x02U) != 0U) {
    clonecore::StableDiskIdentity target;
    if (!read_identity(reader, target)) {
      return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"中断チェックポイントのコピー先識別",
          L"コピー先識別情報が不正です"));
    }
    checkpoint.target = std::move(target);
  }
  if ((flags & 0x04U) != 0U) {
    CheckpointPreparationEvidence evidence;
    std::uint32_t sector_count{};
    if (!reader.read_array(evidence.initial_layout_hash) ||
        !reader.read_u32(evidence.logical_sector_size) ||
        !reader.read_u32(sector_count) || sector_count == 0U ||
        sector_count > kMaximumCheckpointPreparationSectors ||
        static_cast<std::size_t>(sector_count) >
            reader.remaining() / kPreparationSectorWireBytes ||
        reader.remaining() !=
            static_cast<std::size_t>(sector_count) *
                kPreparationSectorWireBytes) {
      return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"中断チェックポイント準備証跡構造",
          L"準備証跡の固定field、sector件数、またはbounded寸法が不正です"));
    }
    evidence.original_sectors.reserve(sector_count);
    for (std::uint32_t index = 0U; index < sector_count; ++index) {
      CheckpointPreparationSectorEvidence sector;
      if (!reader.read_u64(sector.offset) ||
          !reader.read_u64(sector.length) ||
          !reader.read_array(sector.original_hash)) {
        return clonecore::Result<ParsedCheckpoint>::failure(
            checkpoint_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"中断チェックポイント準備sector証跡構造",
                L"準備sector証跡が宣言件数より短いか不正です"));
      }
      evidence.original_sectors.push_back(std::move(sector));
    }
    checkpoint.preparation_evidence = std::move(evidence);
  }
  if ((flags & 0x08U) != 0U) {
    CheckpointOutputProgressEvidence evidence;
    if (!reader.read_array(evidence.verified_prefix_hash) ||
        !reader.read_u64(evidence.primary_output_length) ||
        !reader.read_u64(evidence.journal_length) ||
        !reader.read_u64(evidence.auxiliary_output_length)) {
      return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"中断チェックポイント出力進捗証跡構造",
          L"出力進捗証跡が宣言寸法より短いか不正です"));
    }
    checkpoint.output_progress_evidence = evidence;
  }
  if (!reader.at_end()) {
    return clonecore::Result<ParsedCheckpoint>::failure(checkpoint_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"中断チェックポイント終端",
        L"未解釈または余分なフィールドがあります"));
  }
  const auto valid = validate_checkpoint(checkpoint);
  if (!valid) {
    return clonecore::Result<ParsedCheckpoint>::failure(valid.error());
  }
  return clonecore::Result<ParsedCheckpoint>::success(ParsedCheckpoint{
      .checkpoint = std::move(checkpoint),
      .record_hash = stored_hash,
  });
}

clonecore::Result<std::optional<ParsedCheckpoint>> read_single_checkpoint(
    const std::wstring& path) {
  const auto valid_path = validate_path(path);
  if (!valid_path) {
    return clonecore::Result<std::optional<ParsedCheckpoint>>::failure(
        valid_path.error());
  }
  clonecore::UniqueHandle file;
  return read_existing(path, GENERIC_READ, file);
}

clonecore::Status create_single_checkpoint(
    const std::wstring& path,
    const InterruptionCheckpoint& checkpoint) {
  const auto valid_path = validate_path(path);
  if (!valid_path) {
    return valid_path;
  }
  auto existing = read_single_checkpoint(path);
  if (!existing) {
    // A corrupt or unknown existing file is an intentional fail-closed stop.
    return clonecore::Status::failure(existing.error());
  }
  if (existing.value()) {
    return checkpoint_failure(
        clonecore::ErrorCode::access_denied,
        ERROR_FILE_EXISTS,
        L"中断チェックポイント新規作成",
        L"既存チェックポイントは上書きしません");
  }

  auto bytes = serialize_checkpoint(checkpoint);
  if (!bytes) {
    return clonecore::Status::failure(bytes.error());
  }
  const auto parsed = parse_checkpoint(bytes.value());
  if (!parsed) {
    return clonecore::Status::failure(parsed.error());
  }
  const std::wstring stage_path = path + L".new";
  const auto staged = write_verified_stage(
      stage_path, bytes.value(), parsed.value().record_hash);
  if (!staged) {
    return staged;
  }
  if (!MoveFileExW(
          stage_path.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const DWORD native_code = GetLastError();
    discard_owned_stage_if_matching(stage_path, parsed.value().record_hash);
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"中断チェックポイント新規確定",
        native_code));
  }
  return verify_committed_file(path, parsed.value().record_hash);
}

clonecore::Status replace_single_checkpoint(
    const std::wstring& path,
    const Sha256Digest& expected_current_record_hash,
    const InterruptionCheckpoint& next) {
  const auto valid_path = validate_path(path);
  if (!valid_path) {
    return valid_path;
  }
  auto current = read_single_checkpoint(path);
  if (!current) {
    return clonecore::Status::failure(current.error());
  }
  if (!current.value() ||
      !detail::digest_equal(
          current.value()->record_hash, expected_current_record_hash)) {
    return checkpoint_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"中断チェックポイント置換前検証",
        L"既存ファイルが確認済みチェックポイントと一致しません");
  }
  const auto transition = validate_checkpoint_transition(
      current.value()->checkpoint, next);
  if (!transition) {
    return transition;
  }

  auto bytes = serialize_checkpoint(next);
  if (!bytes) {
    return clonecore::Status::failure(bytes.error());
  }
  const auto next_parsed = parse_checkpoint(bytes.value());
  if (!next_parsed) {
    return clonecore::Status::failure(next_parsed.error());
  }
  const std::wstring stage_path = path + L".new";
  const auto staged = write_verified_stage(
      stage_path, bytes.value(), next_parsed.value().record_hash);
  if (!staged) {
    return staged;
  }

  // Re-read immediately before the atomic replacement. A changed, corrupt,
  // or unknown target remains untouched and the owned stage alone is removed.
  auto last_observation = read_single_checkpoint(path);
  if (!last_observation || !last_observation.value() ||
      !detail::digest_equal(
          last_observation.value()->record_hash,
          expected_current_record_hash)) {
    discard_owned_stage_if_matching(
        stage_path, next_parsed.value().record_hash);
    return checkpoint_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"中断チェックポイント置換直前検証",
        L"置換直前に既存ファイルが変更されました");
  }

  if (!ReplaceFileW(
          path.c_str(),
          stage_path.c_str(),
          nullptr,
          REPLACEFILE_WRITE_THROUGH,
          nullptr,
          nullptr)) {
    const DWORD native_code = GetLastError();
    discard_owned_stage_if_matching(
        stage_path, next_parsed.value().record_hash);
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"中断チェックポイント安全置換",
        native_code));
  }
  return verify_committed_file(path, next_parsed.value().record_hash);
}

clonecore::Status discard_single_checkpoint(
    const std::wstring& path,
    const Sha256Digest& expected_record_hash) {
  const auto valid_path = validate_path(path);
  if (!valid_path) {
    return valid_path;
  }
  clonecore::UniqueHandle file;
  auto existing = read_existing(path, GENERIC_READ | DELETE, file);
  if (!existing) {
    return clonecore::Status::failure(existing.error());
  }
  if (!existing.value() ||
      !detail::digest_equal(
          existing.value()->record_hash, expected_record_hash)) {
    return checkpoint_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"中断チェックポイント破棄前検証",
        L"既存ファイルが確認済みチェックポイントと一致しないため削除しません");
  }
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(
          file.get(),
          FileDispositionInfo,
          &disposition,
          sizeof(disposition))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"中断チェックポイント破棄",
        GetLastError()));
  }
  file.reset();
  return clonecore::success_status();
}

}  // namespace ytec::operationcore

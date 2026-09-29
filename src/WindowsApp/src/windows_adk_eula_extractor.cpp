#include "ytec/windowsapp/windows_adk_eula_extractor.h"

#include "windows_adk_eula_extractor_internal.h"

#include "ytec/clonecore/error.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/sha256.h"

#include <Windows.h>
#include <Objbase.h>
#include <fdi.h>
#pragma warning(disable : 6553)
// Windows SDK 10.0.26100 applies a pointer-only SAL annotation to the
// documented integer LONG_PTR value of IXmlReader::SetProperty. Keep this
// translation-unit-local suppression while still analyzing all product code.
#include <xmllite.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kCabHeaderBytes = 36U;
constexpr std::size_t kIoBlockBytes = 1024U * 1024U;
constexpr std::size_t kMaximumManifestBytes = 1024U * 1024U;
constexpr std::uint64_t kMaximumEulaBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumCabMembers = 256U;
constexpr std::size_t kMaximumCabMemberNameBytes = 128U;
constexpr std::wstring_view kBurnNamespace =
    L"http://schemas.microsoft.com/wix/2008/Burn";
constexpr std::string_view kFdiCabinetName = "owned.cab";

clonecore::Error extractor_error(
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
  return clonecore::Result<T>::failure(extractor_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(extractor_error(
      code, native_code, std::move(operation), std::move(message)));
}

std::uint16_t read_u16(const std::span<const std::byte> bytes) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[0]) |
      (static_cast<std::uint16_t>(
           std::to_integer<std::uint8_t>(bytes[1]))
       << 8U));
}

std::uint32_t read_u32(const std::span<const std::byte> bytes) noexcept {
  return static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(bytes[0]) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(bytes[1]))
       << 8U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(bytes[2]))
       << 16U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(bytes[3]))
       << 24U));
}

bool ascii_hex_sha256(const std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'F');
         });
}

bool bounded_ascii_wide(
    const std::wstring_view value,
    const std::size_t maximum_characters) noexcept {
  return !value.empty() && value.size() <= maximum_characters &&
         std::all_of(value.begin(), value.end(), [](const wchar_t character) {
           return character >= 0x20 && character <= 0x7e;
         });
}

bool exact_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

template <typename Interface>
class ComOwner final {
 public:
  ComOwner() = default;
  ~ComOwner() {
    if (value_ != nullptr) {
      value_->Release();
    }
  }
  ComOwner(const ComOwner&) = delete;
  ComOwner& operator=(const ComOwner&) = delete;
  ComOwner(ComOwner&& other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
  }
  ComOwner& operator=(ComOwner&&) = delete;

  [[nodiscard]] Interface* get() const noexcept { return value_; }
  [[nodiscard]] Interface** put() noexcept { return &value_; }

 private:
  Interface* value_{};
};

clonecore::Result<ComOwner<IStream>> bounded_memory_stream(
    const std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > kMaximumManifestBytes) {
    return failure<ComOwner<IStream>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"ADK EULA Burn manifest入力",
        L"Burn manifestが空か固定上限を超えています");
  }
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (memory == nullptr) {
    return clonecore::Result<ComOwner<IStream>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::internal_error,
            L"ADK EULA Burn manifestメモリ確保",
            GetLastError()));
  }
  void* const locked = GlobalLock(memory);
  if (locked == nullptr) {
    const DWORD native_code = GetLastError();
    GlobalFree(memory);
    return clonecore::Result<ComOwner<IStream>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::internal_error,
            L"ADK EULA Burn manifestメモリ固定",
            native_code));
  }
  std::memcpy(locked, bytes.data(), bytes.size());
  GlobalUnlock(memory);

  ComOwner<IStream> stream;
  const HRESULT created =
      CreateStreamOnHGlobal(memory, TRUE, stream.put());
  if (FAILED(created)) {
    GlobalFree(memory);
    return failure<ComOwner<IStream>>(
        clonecore::ErrorCode::internal_error,
        static_cast<DWORD>(created),
        L"ADK EULA Burn manifest stream作成",
        L"有界メモリstreamを作成できません");
  }
  return clonecore::Result<ComOwner<IStream>>::success(std::move(stream));
}

struct PayloadAttributes final {
  std::wstring source_path;
  std::wstring file_path;
  std::wstring file_size;
  bool source_seen{};
  bool path_seen{};
  bool size_seen{};
  bool duplicate_attribute{};
};

clonecore::Result<PayloadAttributes> read_payload_attributes(
    IXmlReader& reader) {
  PayloadAttributes attributes{};
  HRESULT moved = reader.MoveToFirstAttribute();
  while (moved == S_OK) {
    const wchar_t* local_name{};
    UINT local_length{};
    const wchar_t* value{};
    UINT value_length{};
    if (FAILED(reader.GetLocalName(&local_name, &local_length)) ||
        FAILED(reader.GetValue(&value, &value_length)) ||
        local_name == nullptr || value == nullptr ||
        local_length > 128U || value_length > 512U) {
      return failure<PayloadAttributes>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK EULA Burn manifest属性",
          L"Payload属性を有界文字列として読めません");
    }
    const std::wstring_view name(local_name, local_length);
    const std::wstring copied_value(value, value_length);
    if (name == L"SourcePath") {
      attributes.duplicate_attribute =
          attributes.duplicate_attribute || attributes.source_seen;
      attributes.source_seen = true;
      attributes.source_path = copied_value;
    } else if (name == L"FilePath") {
      attributes.duplicate_attribute =
          attributes.duplicate_attribute || attributes.path_seen;
      attributes.path_seen = true;
      attributes.file_path = copied_value;
    } else if (name == L"FileSize") {
      attributes.duplicate_attribute =
          attributes.duplicate_attribute || attributes.size_seen;
      attributes.size_seen = true;
      attributes.file_size = copied_value;
    }
    moved = reader.MoveToNextAttribute();
  }
  if (moved != S_FALSE || FAILED(reader.MoveToElement())) {
    return failure<PayloadAttributes>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK EULA Burn manifest属性",
        L"Payload属性列を完全に検査できません");
  }
  return clonecore::Result<PayloadAttributes>::success(
      std::move(attributes));
}

std::string digest_to_hex(const imageformat::Sha256Digest& digest) {
  constexpr char hexadecimal[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(64U);
  for (const std::byte value : digest) {
    const unsigned int byte = std::to_integer<unsigned int>(value);
    result.push_back(hexadecimal[(byte >> 4U) & 0x0fU]);
    result.push_back(hexadecimal[byte & 0x0fU]);
  }
  return result;
}

clonecore::Result<std::string> sha256_handle(
    const HANDLE handle,
    const std::uint64_t byte_count,
    const std::wstring_view operation) {
  const auto digest = imageformat::sha256_from_reader(
      byte_count,
      kIoBlockBytes,
      [handle, operation](
          const std::uint64_t offset,
          const std::size_t length)
          -> clonecore::Result<std::vector<std::byte>> {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::io_failed,
                  operation,
                  GetLastError()));
        }
        std::vector<std::byte> result(length);
        std::size_t consumed{};
        while (consumed < result.size()) {
          const DWORD request = static_cast<DWORD>((std::min)(
              result.size() - consumed,
              static_cast<std::size_t>(
                  (std::numeric_limits<DWORD>::max)())));
          DWORD read{};
          if (!ReadFile(
                  handle,
                  result.data() + consumed,
                  request,
                  &read,
                  nullptr) ||
              read == 0U) {
            return failure<std::vector<std::byte>>(
                clonecore::ErrorCode::io_failed,
                read == 0U ? ERROR_HANDLE_EOF : GetLastError(),
                std::wstring(operation),
                L"SHA-256読取り中に固定範囲を完全に読めません");
          }
          consumed += read;
        }
        return clonecore::Result<std::vector<std::byte>>::success(
            std::move(result));
      });
  if (!digest) {
    return clonecore::Result<std::string>::failure(digest.error());
  }
  return clonecore::Result<std::string>::success(
      digest_to_hex(digest.value()));
}

struct NativeObservation final {
  std::uint64_t byte_count{};
  std::uint32_t link_count{};
  std::array<std::byte, 16U> file_id{};
  std::uint64_t volume_serial{};
  bool regular{};
  bool reparse_point{};

  [[nodiscard]] bool same_identity(
      const NativeObservation& other) const noexcept {
    return volume_serial == other.volume_serial && file_id == other.file_id;
  }
};

clonecore::Result<NativeObservation> observe_native_file(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO identity{};
  if (!GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          handle,
          FileStandardInfo,
          &standard,
          sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          handle, FileIdInfo, &identity, sizeof(identity))) {
    return clonecore::Result<NativeObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if (standard.EndOfFile.QuadPart < 0 || standard.NumberOfLinks == 0U) {
    return failure<NativeObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"ファイル長またはリンク数が不正です");
  }
  NativeObservation result{};
  result.byte_count =
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  result.link_count = standard.NumberOfLinks;
  result.volume_serial = identity.VolumeSerialNumber;
  std::memcpy(
      result.file_id.data(),
      identity.FileId.Identifier,
      result.file_id.size());
  result.reparse_point =
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
  result.regular =
      (attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) == 0U;
  return clonecore::Result<NativeObservation>::success(result);
}

clonecore::Status set_delete_disposition(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        operation,
        GetLastError()));
  }
  return clonecore::success_status();
}

class OwnedTemporaryFile final {
 public:
  OwnedTemporaryFile() = default;
  ~OwnedTemporaryFile() noexcept { cleanup_best_effort(); }
  OwnedTemporaryFile(const OwnedTemporaryFile&) = delete;
  OwnedTemporaryFile& operator=(const OwnedTemporaryFile&) = delete;

  [[nodiscard]] clonecore::Status create_new(
      const std::filesystem::path& path,
      const std::wstring_view operation) {
    if (handle_) {
      return status_failure(
          clonecore::ErrorCode::internal_error,
          ERROR_ALREADY_EXISTS,
          std::wstring(operation),
          L"一時ファイルは既に作成済みです");
    }
    clonecore::UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          operation,
          GetLastError()));
    }
    const auto observed = observe_native_file(handle.get(), operation);
    if (!observed || !observed.value().regular ||
        observed.value().reparse_point ||
        observed.value().link_count != 1U ||
        observed.value().byte_count != 0U) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          std::wstring(operation),
          L"CREATE_NEWの空の通常単一リンクファイルではありません");
    }
    path_ = path;
    identity_ = observed.value();
    handle_ = std::move(handle);
    return clonecore::success_status();
  }

  [[nodiscard]] HANDLE handle() const noexcept { return handle_.get(); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }
  [[nodiscard]] bool created() const noexcept {
    return static_cast<bool>(handle_);
  }

  [[nodiscard]] clonecore::Status cleanup() {
    if (!handle_) {
      return clonecore::success_status();
    }
    const auto observed = observe_native_file(
        handle_.get(), L"ADK EULA一時ファイル 削除前再識別");
    if (!observed || !observed.value().regular ||
        observed.value().reparse_point ||
        observed.value().link_count != 1U ||
        !observed.value().same_identity(identity_)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK EULA一時ファイル 削除前再識別",
          L"作成時と同じ通常単一リンクファイルではないため削除しません");
    }
    const auto disposed = set_delete_disposition(
        handle_.get(), L"ADK EULA一時ファイル 安全削除");
    if (!disposed) {
      return disposed;
    }
    handle_.reset();
    const DWORD attributes = GetFileAttributesW(path_.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_ALREADY_EXISTS,
          L"ADK EULA一時ファイル 削除後確認",
          L"削除後に同名の未知オブジェクトが存在するため成功にできません");
    }
    const DWORD native_code = GetLastError();
    if (native_code != ERROR_FILE_NOT_FOUND &&
        native_code != ERROR_PATH_NOT_FOUND) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"ADK EULA一時ファイル 削除後確認",
          native_code));
    }
    path_.clear();
    return clonecore::success_status();
  }

 private:
  void cleanup_best_effort() noexcept {
    if (!handle_) {
      return;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO identity{};
    const bool observed =
        GetFileInformationByHandleEx(
            handle_.get(),
            FileAttributeTagInfo,
            &attributes,
            sizeof(attributes)) &&
        GetFileInformationByHandleEx(
            handle_.get(),
            FileStandardInfo,
            &standard,
            sizeof(standard)) &&
        GetFileInformationByHandleEx(
            handle_.get(), FileIdInfo, &identity, sizeof(identity));
    std::array<std::byte, 16U> file_id{};
    if (observed) {
      std::memcpy(
          file_id.data(), identity.FileId.Identifier, file_id.size());
    }
    if (observed && standard.NumberOfLinks == 1U &&
        identity.VolumeSerialNumber == identity_.volume_serial &&
        file_id == identity_.file_id &&
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE |
          FILE_ATTRIBUTE_REPARSE_POINT)) == 0U) {
      FILE_DISPOSITION_INFO disposition{};
      disposition.DeleteFile = TRUE;
      static_cast<void>(SetFileInformationByHandle(
          handle_.get(),
          FileDispositionInfo,
          &disposition,
          sizeof(disposition)));
    }
  }

  std::filesystem::path path_;
  NativeObservation identity_;
  clonecore::UniqueHandle handle_;
};

clonecore::Result<std::filesystem::path> unique_temporary_path(
    const std::filesystem::path& owned_root,
    const std::wstring_view suffix) {
  for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
    GUID identifier{};
    if (FAILED(CoCreateGuid(&identifier))) {
      return failure<std::filesystem::path>(
          clonecore::ErrorCode::internal_error,
          ERROR_GEN_FAILURE,
          L"ADK EULA一時名生成",
          L"一意な一時ファイル名を生成できません");
    }
    std::array<wchar_t, 40U> text{};
    if (StringFromGUID2(
            identifier,
            text.data(),
            static_cast<int>(text.size())) == 0) {
      return failure<std::filesystem::path>(
          clonecore::ErrorCode::internal_error,
          ERROR_INSUFFICIENT_BUFFER,
          L"ADK EULA一時名生成",
          L"一時ファイル識別子を文字列化できません");
    }
    const std::filesystem::path candidate =
        owned_root / (std::wstring(text.data()) + std::wstring(suffix));
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return clonecore::Result<std::filesystem::path>::success(candidate);
      }
      return clonecore::Result<std::filesystem::path>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"ADK EULA一時名 衝突検査",
              native_code));
    }
  }
  return failure<std::filesystem::path>(
      clonecore::ErrorCode::io_failed,
      ERROR_ALREADY_EXISTS,
      L"ADK EULA一時名 衝突検査",
      L"未使用の一時ファイル名を確保できません");
}

clonecore::Status seek_absolute(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::wstring_view operation) {
  if (offset > static_cast<std::uint64_t>(
                   (std::numeric_limits<LONGLONG>::max)())) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"ファイル位置がWindows境界を超えています");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        operation,
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Status read_exact(
    const HANDLE handle,
    const std::span<std::byte> output,
    const std::wstring_view operation) {
  std::size_t consumed{};
  while (consumed < output.size()) {
    const DWORD request = static_cast<DWORD>((std::min)(
        output.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    if (!ReadFile(
            handle,
            output.data() + consumed,
            request,
            &read,
            nullptr) ||
        read == 0U) {
      return status_failure(
          clonecore::ErrorCode::io_failed,
          read == 0U ? ERROR_HANDLE_EOF : GetLastError(),
          std::wstring(operation),
          L"固定範囲を完全に読み取れません");
    }
    consumed += read;
  }
  return clonecore::success_status();
}

clonecore::Status write_exact(
    const HANDLE handle,
    const std::span<const std::byte> input,
    const std::wstring_view operation) {
  std::size_t consumed{};
  while (consumed < input.size()) {
    const DWORD request = static_cast<DWORD>((std::min)(
        input.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written{};
    if (!WriteFile(
            handle,
            input.data() + consumed,
            request,
            &written,
            nullptr) ||
        written == 0U) {
      return status_failure(
          clonecore::ErrorCode::io_failed,
          written == 0U ? ERROR_WRITE_FAULT : GetLastError(),
          std::wstring(operation),
          L"固定範囲を完全に書き込めません");
    }
    consumed += written;
  }
  return clonecore::success_status();
}

clonecore::Status copy_and_readback_container(
    const HANDLE source,
    const HANDLE destination,
    const AdkEmbeddedEulaPin& pin) {
  auto status = seek_absolute(
      source, pin.container_offset, L"ADK EULA付属CAB コピー元位置決め");
  if (!status) {
    return status;
  }
  status = seek_absolute(
      destination, 0U, L"ADK EULA付属CAB 保存先位置決め");
  if (!status) {
    return status;
  }
  std::vector<std::byte> buffer(kIoBlockBytes);
  std::uint64_t remaining = pin.container_length;
  while (remaining != 0U) {
    const std::size_t amount = static_cast<std::size_t>((std::min)(
        remaining, static_cast<std::uint64_t>(buffer.size())));
    status = read_exact(
        source,
        std::span<std::byte>(buffer.data(), amount),
        L"ADK EULA付属CAB 有界コピー元読取り");
    if (!status) {
      return status;
    }
    status = write_exact(
        destination,
        std::span<const std::byte>(buffer.data(), amount),
        L"ADK EULA付属CAB CREATE_NEW書込み");
    if (!status) {
      return status;
    }
    remaining -= amount;
  }
  if (!FlushFileBuffers(destination)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"ADK EULA付属CAB flush",
        GetLastError()));
  }

  status = seek_absolute(
      source,
      pin.container_offset,
      L"ADK EULA付属CAB 読戻し元位置決め");
  if (!status) {
    return status;
  }
  status = seek_absolute(
      destination, 0U, L"ADK EULA付属CAB 読戻し位置決め");
  if (!status) {
    return status;
  }
  std::vector<std::byte> copied(kIoBlockBytes);
  remaining = pin.container_length;
  while (remaining != 0U) {
    const std::size_t amount = static_cast<std::size_t>((std::min)(
        remaining, static_cast<std::uint64_t>(buffer.size())));
    status = read_exact(
        source,
        std::span<std::byte>(buffer.data(), amount),
        L"ADK EULA付属CAB 読戻し元読取り");
    if (!status) {
      return status;
    }
    status = read_exact(
        destination,
        std::span<std::byte>(copied.data(), amount),
        L"ADK EULA付属CAB 読戻し");
    if (!status) {
      return status;
    }
    if (!std::equal(
            buffer.begin(),
            buffer.begin() + static_cast<std::ptrdiff_t>(amount),
            copied.begin())) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"ADK EULA付属CAB 読戻し比較",
          L"CREATE_NEWへ保存したCABが元の固定範囲と一致しません");
    }
    remaining -= amount;
  }
  return clonecore::success_status();
}

enum class FdiFileKind : std::uint8_t {
  cabinet,
  manifest,
  eula,
};

struct FdiContext;

struct FdiFile final {
  FdiFileKind kind{FdiFileKind::cabinet};
  FdiContext* context{};
  HANDLE handle{INVALID_HANDLE_VALUE};
  std::vector<std::byte>* memory{};
  std::uint64_t position{};
  std::uint64_t limit{};
};

struct FdiContext final {
  HANDLE cabinet_handle{INVALID_HANDLE_VALUE};
  std::uint64_t cabinet_bytes{};
  OwnedTemporaryFile* eula_file{};
  std::filesystem::path eula_path;
  std::uint64_t expected_eula_bytes{};
  std::vector<std::byte> manifest;
  std::set<std::string> lower_member_names;
  std::size_t notification_count{};
  bool failed{};
  DWORD failure_code{ERROR_INVALID_DATA};
  std::wstring failure_message;
  bool manifest_seen{};
  bool manifest_closed{};
  bool eula_seen{};
  bool eula_closed{};
};

thread_local FdiContext* g_fdi_context{};

void fdi_fail(
    FdiContext& context,
    const DWORD native_code,
    std::wstring message) {
  context.failed = true;
  context.failure_code = native_code;
  context.failure_message = std::move(message);
}

void* DIAMONDAPI fdi_alloc(const ULONG bytes) {
  return std::malloc(bytes);
}

void DIAMONDAPI fdi_free(void* const memory) {
  std::free(memory);
}

INT_PTR DIAMONDAPI fdi_open(
    char* const file_name,
    const int open_flags,
    const int permission_mode) {
  static_cast<void>(permission_mode);
  FdiContext* const context = g_fdi_context;
  if (context == nullptr || context->failed || file_name == nullptr ||
      std::string_view(file_name) != kFdiCabinetName ||
      (open_flags & (_O_WRONLY | _O_RDWR)) != 0) {
    if (context != nullptr) {
      fdi_fail(
          *context,
          ERROR_INVALID_NAME,
          L"Cabinet APIが固定CAB以外を開こうとしました");
    }
    return -1;
  }
  HANDLE duplicate{INVALID_HANDLE_VALUE};
  if (!DuplicateHandle(
          GetCurrentProcess(),
          context->cabinet_handle,
          GetCurrentProcess(),
          &duplicate,
          0,
          FALSE,
          DUPLICATE_SAME_ACCESS)) {
    fdi_fail(
        *context,
        GetLastError(),
        L"Cabinet API向け同一CAB handleを複製できません");
    return -1;
  }
  auto file = std::make_unique<FdiFile>();
  file->kind = FdiFileKind::cabinet;
  file->context = context;
  file->handle = duplicate;
  file->limit = context->cabinet_bytes;
  return reinterpret_cast<INT_PTR>(file.release());
}

UINT DIAMONDAPI fdi_read(
    const INT_PTR opaque,
    void* const output,
    const UINT bytes) {
  auto* const file = reinterpret_cast<FdiFile*>(opaque);
  if (file == nullptr || file->context == nullptr ||
      file->context->failed || (output == nullptr && bytes != 0U) ||
      file->kind != FdiFileKind::cabinet ||
      file->handle == INVALID_HANDLE_VALUE ||
      file->position > file->limit) {
    return static_cast<UINT>(-1);
  }
  const DWORD bounded_bytes = static_cast<DWORD>((std::min)(
      static_cast<std::uint64_t>(bytes),
      file->limit - file->position));
  LARGE_INTEGER absolute{};
  absolute.QuadPart = static_cast<LONGLONG>(file->position);
  if (!SetFilePointerEx(
          file->handle, absolute, nullptr, FILE_BEGIN)) {
    fdi_fail(
        *file->context,
        GetLastError(),
        L"Cabinet APIの同一file-object読取り位置を固定できません");
    return static_cast<UINT>(-1);
  }
  DWORD read{};
  if (!ReadFile(file->handle, output, bounded_bytes, &read, nullptr)) {
    fdi_fail(
        *file->context,
        GetLastError(),
        L"Cabinet APIが固定CABを読み取れません");
    return static_cast<UINT>(-1);
  }
  file->position += read;
  return read;
}

UINT DIAMONDAPI fdi_write(
    const INT_PTR opaque,
    void* const input,
    const UINT bytes) {
  auto* const file = reinterpret_cast<FdiFile*>(opaque);
  if (file == nullptr || file->context == nullptr ||
      file->context->failed || (input == nullptr && bytes != 0U) ||
      file->kind == FdiFileKind::cabinet ||
      file->position > file->limit ||
      bytes > file->limit - file->position) {
    if (file != nullptr && file->context != nullptr) {
      fdi_fail(
          *file->context,
          ERROR_FILE_TOO_LARGE,
          L"Cabinet memberの展開長が固定上限を超えました");
    }
    return static_cast<UINT>(-1);
  }
  if (file->kind == FdiFileKind::manifest) {
    if (file->memory == nullptr || file->position != file->memory->size()) {
      fdi_fail(
          *file->context,
          ERROR_INVALID_DATA,
          L"Burn manifestが非連続に展開されました");
      return static_cast<UINT>(-1);
    }
    if (bytes != 0U) {
      const auto* const first = static_cast<const std::byte*>(input);
      file->memory->insert(file->memory->end(), first, first + bytes);
    }
  } else {
    DWORD written{};
    if (!WriteFile(file->handle, input, bytes, &written, nullptr) ||
        written != bytes) {
      fdi_fail(
          *file->context,
          GetLastError(),
          L"EULA memberをCREATE_NEWへ完全に書き込めません");
      return static_cast<UINT>(-1);
    }
  }
  file->position += bytes;
  return bytes;
}

int DIAMONDAPI fdi_close(const INT_PTR opaque) {
  std::unique_ptr<FdiFile> file(reinterpret_cast<FdiFile*>(opaque));
  if (!file) {
    return -1;
  }
  if (file->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(file->handle);
    file->handle = INVALID_HANDLE_VALUE;
  }
  return 0;
}

long DIAMONDAPI fdi_seek(
    const INT_PTR opaque,
    const long distance,
    const int origin) {
  auto* const file = reinterpret_cast<FdiFile*>(opaque);
  if (file == nullptr || file->context == nullptr ||
      file->context->failed) {
    return -1;
  }
  if (file->kind != FdiFileKind::cabinet) {
    // FDI writes target files sequentially. Seeking an output member is not
    // part of this product's extraction contract.
    fdi_fail(
        *file->context,
        ERROR_INVALID_DATA,
        L"Cabinet APIが展開先memberを非連続に操作しました");
    return -1;
  }
  LARGE_INTEGER move{};
  move.QuadPart = distance;
  LARGE_INTEGER result{};
  const DWORD method = origin == SEEK_SET
                           ? FILE_BEGIN
                           : origin == SEEK_CUR
                                 ? FILE_CURRENT
                                 : origin == SEEK_END ? FILE_END : MAXDWORD;
  if (method == MAXDWORD ||
      !SetFilePointerEx(file->handle, move, &result, method) ||
      result.QuadPart < 0 ||
      result.QuadPart > (std::numeric_limits<long>::max)() ||
      static_cast<std::uint64_t>(result.QuadPart) > file->limit) {
    fdi_fail(
        *file->context,
        result.QuadPart > (std::numeric_limits<long>::max)() ||
                (result.QuadPart >= 0 &&
                 static_cast<std::uint64_t>(result.QuadPart) > file->limit)
            ? ERROR_ARITHMETIC_OVERFLOW
            : GetLastError(),
        L"Cabinet APIの固定CAB位置決めに失敗しました");
    return -1;
  }
  file->position = static_cast<std::uint64_t>(result.QuadPart);
  return static_cast<long>(result.QuadPart);
}

bool safe_member_name(const std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaximumCabMemberNameBytes ||
      name == "." || name == "..") {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](const char character) {
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_' ||
           character == '-' || character == '.';
  });
}

std::string ascii_lower(std::string value) {
  std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](const char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
      });
  return value;
}

INT_PTR make_manifest_output(FdiContext& context, const std::uint64_t bytes) {
  context.manifest.clear();
  context.manifest.reserve(static_cast<std::size_t>(bytes));
  auto file = std::make_unique<FdiFile>();
  file->kind = FdiFileKind::manifest;
  file->context = &context;
  file->memory = &context.manifest;
  file->limit = bytes;
  return reinterpret_cast<INT_PTR>(file.release());
}

INT_PTR make_eula_output(FdiContext& context, const std::uint64_t bytes) {
  const auto created = context.eula_file->create_new(
      context.eula_path, L"ADK EULA member CREATE_NEW");
  if (!created) {
    fdi_fail(
        context,
        created.error().native_code,
        L"EULA memberのCREATE_NEWに失敗しました");
    return -1;
  }
  HANDLE duplicate{INVALID_HANDLE_VALUE};
  if (!DuplicateHandle(
          GetCurrentProcess(),
          context.eula_file->handle(),
          GetCurrentProcess(),
          &duplicate,
          0,
          FALSE,
          DUPLICATE_SAME_ACCESS)) {
    fdi_fail(
        context,
        GetLastError(),
        L"EULA memberの同一file-object handleを複製できません");
    return -1;
  }
  auto file = std::make_unique<FdiFile>();
  file->kind = FdiFileKind::eula;
  file->context = &context;
  file->handle = duplicate;
  file->limit = bytes;
  return reinterpret_cast<INT_PTR>(file.release());
}

INT_PTR DIAMONDAPI fdi_notify(
    const FDINOTIFICATIONTYPE type,
    PFDINOTIFICATION notification) {
  auto* const context = notification == nullptr
                            ? nullptr
                            : static_cast<FdiContext*>(notification->pv);
  if (context == nullptr || context->failed) {
    return -1;
  }
  switch (type) {
    case fdintCOPY_FILE: {
      if (notification->psz1 == nullptr || notification->cb < 0 ||
          ++context->notification_count > kMaximumCabMembers) {
        fdi_fail(
            *context,
            ERROR_INVALID_DATA,
            L"Cabinet memberの名前、長さ、または個数が不正です");
        return -1;
      }
      const std::size_t length = strnlen_s(
          notification->psz1, kMaximumCabMemberNameBytes + 1U);
      if (length == 0U || length > kMaximumCabMemberNameBytes) {
        fdi_fail(
            *context,
            ERROR_INVALID_NAME,
            L"Cabinet member名が有界NUL終端文字列ではありません");
        return -1;
      }
      const std::string name(notification->psz1, length);
      const std::string lower = ascii_lower(name);
      if (!safe_member_name(name) ||
          !context->lower_member_names.insert(lower).second) {
        fdi_fail(
            *context,
            ERROR_INVALID_NAME,
            L"Cabinet memberにpath traversalまたは重複名があります");
        return -1;
      }
      const std::uint64_t bytes =
          static_cast<std::uint64_t>(notification->cb);
      if (lower == "0") {
        if (name != "0" || context->manifest_seen || bytes == 0U ||
            bytes > kMaximumManifestBytes) {
          fdi_fail(
              *context,
              ERROR_INVALID_DATA,
              L"Burn manifest member 0の名前、個数、または長さが不正です");
          return -1;
        }
        context->manifest_seen = true;
        return make_manifest_output(*context, bytes);
      }
      if (lower == "u6") {
        if (name != "u6" || context->eula_seen || bytes == 0U ||
            bytes > kMaximumEulaBytes ||
            bytes != context->expected_eula_bytes) {
          fdi_fail(
              *context,
              ERROR_INVALID_DATA,
              L"EULA member u6の名前、個数、または長さが不正です");
          return -1;
        }
        context->eula_seen = true;
        return make_eula_output(*context, bytes);
      }
      return 0;
    }
    case fdintCLOSE_FILE_INFO: {
      auto* const file = reinterpret_cast<FdiFile*>(notification->hf);
      if (file == nullptr || file->context != context ||
          (file->kind != FdiFileKind::manifest &&
           file->kind != FdiFileKind::eula) ||
          file->position != file->limit) {
        fdi_fail(
            *context,
            ERROR_INVALID_DATA,
            L"Cabinet memberを固定長どおり閉じられません");
        if (file != nullptr) {
          static_cast<void>(fdi_close(notification->hf));
        }
        return -1;
      }
      if (file->kind == FdiFileKind::eula &&
          !FlushFileBuffers(file->handle)) {
        fdi_fail(
            *context,
            GetLastError(),
            L"EULA memberを同一file-objectへflushできません");
        static_cast<void>(fdi_close(notification->hf));
        return -1;
      }
      if (file->kind == FdiFileKind::manifest) {
        context->manifest_closed = true;
      } else {
        context->eula_closed = true;
      }
      static_cast<void>(fdi_close(notification->hf));
      return TRUE;
    }
    case fdintNEXT_CABINET:
      fdi_fail(
          *context,
          ERROR_NOT_SUPPORTED,
          L"複数CABまたは継続CABは許可されていません");
      return -1;
    case fdintCABINET_INFO:
      if (notification->iCabinet != 0U) {
        fdi_fail(
            *context,
            ERROR_NOT_SUPPORTED,
            L"固定CAB以外のcabinet indexが通知されました");
        return -1;
      }
      return 0;
    case fdintPARTIAL_FILE:
      fdi_fail(
          *context,
          ERROR_INVALID_DATA,
          L"継続memberを含むCABは許可されていません");
      return -1;
    case fdintENUMERATE:
      return 0;
    default:
      fdi_fail(
          *context,
          ERROR_INVALID_DATA,
          L"未知のCabinet通知を受け取りました");
      return -1;
  }
}

class FdiOwner final {
 public:
  explicit FdiOwner(const HFDI value) noexcept : value_(value) {}
  ~FdiOwner() {
    if (value_ != nullptr) {
      static_cast<void>(FDIDestroy(value_));
    }
  }
  FdiOwner(const FdiOwner&) = delete;
  FdiOwner& operator=(const FdiOwner&) = delete;
  [[nodiscard]] HFDI get() const noexcept { return value_; }

 private:
  HFDI value_{};
};

class ActiveFdiContext final {
 public:
  explicit ActiveFdiContext(FdiContext& context) : context_(&context) {
    if (g_fdi_context != nullptr) {
      fdi_fail(
          context,
          ERROR_BUSY,
          L"Cabinet extractorの再入呼出しは許可されていません");
      return;
    }
    g_fdi_context = &context;
    active_ = true;
  }
  ~ActiveFdiContext() {
    if (active_ && g_fdi_context == context_) {
      g_fdi_context = nullptr;
    }
  }
  ActiveFdiContext(const ActiveFdiContext&) = delete;
  ActiveFdiContext& operator=(const ActiveFdiContext&) = delete;
  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  FdiContext* context_{};
  bool active_{};
};

clonecore::Status extract_pinned_members_with_fdi(
    const HANDLE cabinet_handle,
    const WindowsAdkEmbeddedCabHeader& parsed_header,
    const AdkEmbeddedEulaPin& pin,
    OwnedTemporaryFile& eula_file,
    const std::filesystem::path& eula_path,
    std::vector<std::byte>& manifest) {
  FdiContext context{};
  context.cabinet_handle = cabinet_handle;
  context.cabinet_bytes = pin.container_length;
  context.eula_file = &eula_file;
  context.eula_path = eula_path;
  context.expected_eula_bytes = pin.expected_byte_count;
  ActiveFdiContext active(context);
  if (!active.active()) {
    return status_failure(
        clonecore::ErrorCode::access_denied,
        context.failure_code,
        L"ADK EULA Cabinet extractor",
        context.failure_message);
  }

  ERF errors{};
  FdiOwner fdi(FDICreate(
      fdi_alloc,
      fdi_free,
      fdi_open,
      fdi_read,
      fdi_write,
      fdi_close,
      fdi_seek,
      cpuUNKNOWN,
      &errors));
  if (fdi.get() == nullptr) {
    return status_failure(
        clonecore::ErrorCode::internal_error,
        static_cast<DWORD>(errors.erfOper),
        L"ADK EULA Cabinet API初期化",
        L"FDICreateが失敗しました");
  }

  HANDLE cabinet_duplicate{INVALID_HANDLE_VALUE};
  if (!DuplicateHandle(
          GetCurrentProcess(),
          cabinet_handle,
          GetCurrentProcess(),
          &cabinet_duplicate,
          0,
          FALSE,
          DUPLICATE_SAME_ACCESS)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"ADK EULA Cabinet API構造検査handle",
        GetLastError()));
  }
  auto cabinet_file = std::make_unique<FdiFile>();
  cabinet_file->kind = FdiFileKind::cabinet;
  cabinet_file->context = &context;
  cabinet_file->handle = cabinet_duplicate;
  cabinet_file->limit = pin.container_length;
  FDICABINETINFO info{};
  const INT_PTR cabinet_test_handle =
      reinterpret_cast<INT_PTR>(cabinet_file.release());
  const BOOL is_cabinet =
      FDIIsCabinet(fdi.get(), cabinet_test_handle, &info);
  static_cast<void>(fdi_close(cabinet_test_handle));
  if (!is_cabinet) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        static_cast<DWORD>(errors.erfOper),
        L"ADK EULA Cabinet API構造検査",
        L"固定範囲を単一CABとして検証できません");
  }
  if (info.cbCabinet < 0 ||
      static_cast<std::uint32_t>(info.cbCabinet) !=
          parsed_header.cabinet_byte_count ||
      info.cFolders != parsed_header.folder_count ||
      info.cFiles != parsed_header.file_count ||
      info.setID != parsed_header.set_id ||
      info.iCabinet != parsed_header.cabinet_index ||
      info.fReserve || info.hasprev || info.hasnext) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA Cabinet API構造照合",
        L"FDIIsCabinet結果が固定MSCF headerと一致しません");
  }

  char cabinet_name[] = "owned.cab";
  char cabinet_path[] = "";
  if (!FDICopy(
          fdi.get(),
          cabinet_name,
          cabinet_path,
          0,
          fdi_notify,
          nullptr,
          &context)) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        context.failed ? context.failure_code
                       : static_cast<DWORD>(errors.erfOper),
        L"ADK EULA Cabinet member有界抽出",
        context.failed ? context.failure_message
                       : L"FDICopyが固定CABの検証抽出に失敗しました");
  }
  if (context.failed || !context.manifest_seen ||
      !context.manifest_closed || !context.eula_seen ||
      !context.eula_closed ||
      context.notification_count != parsed_header.file_count ||
      context.lower_member_names.size() != parsed_header.file_count ||
      context.manifest.empty() || !eula_file.created()) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        context.failure_code,
        L"ADK EULA Cabinet member完了検査",
        context.failed
            ? context.failure_message
            : L"member列挙、重複検査、または対象member展開が完了していません");
  }
  const auto eula_observation = observe_native_file(
      eula_file.handle(), L"ADK EULA member 展開直後識別");
  if (!eula_observation || !eula_observation.value().regular ||
      eula_observation.value().reparse_point ||
      eula_observation.value().link_count != 1U ||
      eula_observation.value().byte_count != pin.expected_byte_count) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA member 展開直後識別",
        L"u6が固定長の通常単一リンクfile-objectではありません");
  }
  manifest = std::move(context.manifest);
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_whole_handle(
    const HANDLE handle,
    const std::uint64_t byte_count,
    const std::wstring_view operation) {
  if (byte_count == 0U ||
      byte_count > static_cast<std::uint64_t>(
                       (std::numeric_limits<std::size_t>::max)())) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        std::wstring(operation),
        L"読取り長が固定メモリ境界外です");
  }
  auto status = seek_absolute(handle, 0U, operation);
  if (!status) {
    return clonecore::Result<std::vector<std::byte>>::failure(status.error());
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(byte_count));
  status = read_exact(handle, bytes, operation);
  if (!status) {
    return clonecore::Result<std::vector<std::byte>>::failure(status.error());
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

bool contains_ascii_title(
    const std::span<const std::byte> document,
    const std::wstring_view title) {
  if (!bounded_ascii_wide(title, 256U)) {
    return false;
  }
  std::vector<std::byte> bytes;
  bytes.reserve(title.size());
  for (const wchar_t character : title) {
    bytes.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return std::search(
             document.begin(), document.end(), bytes.begin(), bytes.end()) !=
         document.end();
}

}  // namespace

clonecore::Result<WindowsAdkEmbeddedCabHeader>
inspect_windows_adk_embedded_cab_header(
    const std::span<const std::byte> header,
    const std::uint64_t expected_container_bytes) {
  if (header.size() < kCabHeaderBytes ||
      expected_container_bytes < kCabHeaderBytes ||
      expected_container_bytes >
          static_cast<std::uint64_t>((std::numeric_limits<long>::max)()) ||
      std::to_integer<unsigned char>(header[0]) != 'M' ||
      std::to_integer<unsigned char>(header[1]) != 'S' ||
      std::to_integer<unsigned char>(header[2]) != 'C' ||
      std::to_integer<unsigned char>(header[3]) != 'F') {
    return failure<WindowsAdkEmbeddedCabHeader>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK EULA付属CAB MSCF header",
        L"固定長のMSCF headerではありません");
  }
  const WindowsAdkEmbeddedCabHeader result{
      .cabinet_byte_count = read_u32(header.subspan(8U, 4U)),
      .files_offset = read_u32(header.subspan(16U, 4U)),
      .folder_count = read_u16(header.subspan(26U, 2U)),
      .file_count = read_u16(header.subspan(28U, 2U)),
      .flags = read_u16(header.subspan(30U, 2U)),
      .set_id = read_u16(header.subspan(32U, 2U)),
      .cabinet_index = read_u16(header.subspan(34U, 2U)),
      .version_minor = std::to_integer<std::uint8_t>(header[24U]),
      .version_major = std::to_integer<std::uint8_t>(header[25U]),
  };
  if (read_u32(header.subspan(4U, 4U)) != 0U ||
      read_u32(header.subspan(12U, 4U)) != 0U ||
      read_u32(header.subspan(20U, 4U)) != 0U ||
      result.cabinet_byte_count != expected_container_bytes ||
      result.files_offset < kCabHeaderBytes ||
      result.files_offset >= result.cabinet_byte_count ||
      result.version_major != 1U || result.version_minor != 3U ||
      result.folder_count == 0U || result.folder_count > 64U ||
      result.file_count == 0U || result.file_count > kMaximumCabMembers ||
      result.flags != 0U || result.cabinet_index != 0U) {
    return failure<WindowsAdkEmbeddedCabHeader>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA付属CAB MSCF境界検査",
        L"CAB長、予約値、版、個数、flags、またはcabinet indexが固定境界外です");
  }
  return clonecore::Result<WindowsAdkEmbeddedCabHeader>::success(result);
}

clonecore::Status validate_windows_adk_burn_eula_mapping(
    const std::span<const std::byte> burn_manifest,
    const AdkEmbeddedEulaPin& pin) {
  if (burn_manifest.empty() ||
      burn_manifest.size() > kMaximumManifestBytes ||
      !bounded_ascii_wide(pin.container_member_name, 64U) ||
      !bounded_ascii_wide(pin.display_file_name.native(), 260U) ||
      pin.expected_byte_count == 0U ||
      pin.expected_byte_count > kMaximumEulaBytes) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK EULA Burn mapping固定値",
        L"manifestまたはEULA mapping pinが固定境界外です");
  }
  const auto stream = bounded_memory_stream(burn_manifest);
  if (!stream) {
    return clonecore::Status::failure(stream.error());
  }
  ComOwner<IXmlReader> reader;
  HRESULT result = CreateXmlReader(
      __uuidof(IXmlReader),
      reinterpret_cast<void**>(reader.put()),
      nullptr);
  if (FAILED(result) || reader.get() == nullptr) {
    return status_failure(
        clonecore::ErrorCode::internal_error,
        static_cast<DWORD>(result),
        L"ADK EULA Burn manifest XmlLite初期化",
        L"Windows XmlLite readerを初期化できません");
  }
  result = reader.get()->SetProperty(
      XmlReaderProperty_DtdProcessing,
      static_cast<LONG_PTR>(DtdProcessing_Prohibit));
  if (SUCCEEDED(result)) {
    result = reader.get()->SetProperty(
        XmlReaderProperty_MaxElementDepth, 64);
  }
  if (SUCCEEDED(result)) {
    result = reader.get()->SetInput(stream.value().get());
  }
  if (FAILED(result)) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        static_cast<DWORD>(result),
        L"ADK EULA Burn manifest XmlLite安全設定",
        L"DTD禁止または最大depthを有効にできません");
  }

  const std::wstring expected_source = pin.container_member_name;
  const std::wstring expected_path = pin.display_file_name.native();
  const std::wstring expected_size =
      std::to_wstring(pin.expected_byte_count);
  std::size_t exact_mapping_count{};
  XmlNodeType node_type{};
  while ((result = reader.get()->Read(&node_type)) == S_OK) {
    if (node_type == XmlNodeType_DocumentType) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_XML_PARSE_ERROR,
          L"ADK EULA Burn manifest DTD拒否",
          L"Burn manifestにDTD宣言が含まれています");
    }
    if (node_type != XmlNodeType_Element) {
      continue;
    }
    const wchar_t* local_name{};
    UINT local_length{};
    const wchar_t* namespace_uri{};
    UINT namespace_length{};
    if (FAILED(reader.get()->GetLocalName(&local_name, &local_length)) ||
        FAILED(reader.get()->GetNamespaceUri(
            &namespace_uri, &namespace_length)) ||
        local_name == nullptr || namespace_uri == nullptr) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_XML_PARSE_ERROR,
          L"ADK EULA Burn manifest element",
          L"element名またはnamespaceを取得できません");
    }
    if (std::wstring_view(local_name, local_length) != L"Payload") {
      continue;
    }
    const std::wstring element_namespace(namespace_uri, namespace_length);
    const auto attributes = read_payload_attributes(*reader.get());
    if (!attributes) {
      return clonecore::Status::failure(attributes.error());
    }
    const bool source_alias =
        attributes.value().source_seen &&
        exact_case_insensitive(
            attributes.value().source_path, expected_source);
    const bool path_alias =
        attributes.value().path_seen &&
        exact_case_insensitive(
            attributes.value().file_path, expected_path);
    if (!source_alias && !path_alias) {
      continue;
    }
    if (attributes.value().duplicate_attribute ||
        element_namespace != kBurnNamespace ||
        !attributes.value().source_seen ||
        !attributes.value().path_seen || !attributes.value().size_seen ||
        attributes.value().source_path != expected_source ||
        attributes.value().file_path != expected_path ||
        attributes.value().file_size != expected_size) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK EULA Burn mapping厳密照合",
          L"u6、ja\\eula.rtf、293766の対応が改変または曖昧です");
    }
    ++exact_mapping_count;
    if (exact_mapping_count > 1U) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_DUP_NAME,
          L"ADK EULA Burn mapping一意性",
          L"EULA対象のPayload mappingが重複しています");
    }
  }
  if (result != S_FALSE || exact_mapping_count != 1U) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        result == S_FALSE ? ERROR_NOT_FOUND : static_cast<DWORD>(result),
        L"ADK EULA Burn manifest完全検査",
        result == S_FALSE
            ? L"EULA対象の一意なPayload mappingがありません"
            : L"Burn manifestが不正または途中で終了しました");
  }
  return clonecore::success_status();
}

clonecore::Result<AdkEulaDocumentReceipt>
finalize_windows_adk_eula_receipt(
    const AdkEmbeddedEulaPin& pin,
    const WindowsAdkEulaExtractionObservation& observation) {
  if (!pin.primary_source_confirmed ||
      !ascii_hex_sha256(pin.expected_sha256) ||
      !observation.bootstrap_full_identity_verified ||
      !observation.attached_container_created_new ||
      !observation.attached_container_regular_file ||
      !observation.attached_container_single_link ||
      observation.attached_container_reparse_point ||
      !observation.attached_container_bounds_verified ||
      !observation.attached_container_readback_equal ||
      !observation.cabinet_api_structure_verified ||
      !observation.burn_manifest_bounded ||
      !observation.burn_ux_mapping_verified ||
      !observation.member_copy_created_new ||
      !observation.member_copy_regular_file ||
      !observation.member_copy_single_link ||
      observation.member_copy_reparse_point ||
      observation.member_byte_count != pin.expected_byte_count ||
      observation.member_sha256 != pin.expected_sha256 ||
      !observation.member_title_verified ||
      !observation.bounded_read_complete ||
      !observation.temporary_files_removed) {
    return failure<AdkEulaDocumentReceipt>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA抽出receipt最終化",
        L"同一file-object、CAB、mapping、member、Hash、title、またはcleanupの必須検証が未完了です");
  }
  return clonecore::Result<AdkEulaDocumentReceipt>::success(
      AdkEulaDocumentReceipt{
          .extracted_identity = pin,
          .bootstrap_full_identity_verified = true,
          .attached_container_bounds_verified = true,
          .burn_ux_mapping_verified = true,
          .member_copy_created_new = true,
          .member_copy_regular_file = true,
          .member_copy_single_link = true,
          .member_copy_reparse_point = false,
          .bounded_read_complete = true,
          .temporary_files_removed = true,
      });
}

clonecore::Status validate_windows_adk_eula_source_identity(
    const AdkPinnedPayload& pinned_bootstrap,
    const AdkVerifiedPayload& verified_bootstrap,
    const AdkEmbeddedEulaPin& pin) {
  const bool range_valid =
      pin.container_offset <= pinned_bootstrap.expected_byte_count &&
      pin.container_length <=
          pinned_bootstrap.expected_byte_count - pin.container_offset;
  if (pinned_bootstrap.kind != AdkPayloadKind::deployment_tools ||
      pinned_bootstrap.installer_kind !=
          AdkInstallerKind::microsoft_bootstrap_exe ||
      pin.source_payload_kind != pinned_bootstrap.kind ||
      !pin.primary_source_confirmed ||
      pin.official_bootstrap_url != pinned_bootstrap.exact_source_url ||
      pin.container_offset == 0U || pin.container_length < kCabHeaderBytes ||
      !range_valid || pin.container_member_name != L"u6" ||
      pin.display_file_name != std::filesystem::path(L"ja\\eula.rtf") ||
      pin.expected_byte_count == 0U ||
      pin.expected_byte_count > kMaximumEulaBytes ||
      !ascii_hex_sha256(pin.expected_sha256) ||
      !bounded_ascii_wide(pin.expected_document_title, 256U) ||
      verified_bootstrap.kind != pinned_bootstrap.kind ||
      verified_bootstrap.installer_kind != pinned_bootstrap.installer_kind ||
      verified_bootstrap.byte_count != pinned_bootstrap.expected_byte_count ||
      verified_bootstrap.sha256 != pinned_bootstrap.expected_sha256 ||
      verified_bootstrap.signer_subject !=
          pinned_bootstrap.expected_signer_subject ||
      verified_bootstrap.payload_version !=
          pinned_bootstrap.expected_payload_version ||
      !verified_bootstrap.msp_revision_guid.empty()) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA bootstrap固定identity",
        L"Deployment Tools bootstrap、全体Hash、署名者、版、または埋込みEULA pinが一致しません");
  }
  return clonecore::success_status();
}

namespace detail {

clonecore::Result<WindowsAdkEulaExtractionResult>
extract_windows_adk_eula_from_verified_owned_handle(
    const HANDLE source_handle,
    const std::filesystem::path& owned_stage_root,
    const AdkPinnedPayload& pinned_bootstrap,
    const AdkVerifiedPayload& verified_bootstrap,
    const AdkEmbeddedEulaPin& pin) {
  const std::uint64_t source_byte_count = verified_bootstrap.byte_count;
  if (source_handle == nullptr || source_handle == INVALID_HANDLE_VALUE ||
      owned_stage_root.empty() || pin.container_offset > source_byte_count ||
      pin.container_length > source_byte_count - pin.container_offset) {
    return failure<WindowsAdkEulaExtractionResult>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK EULA同一file-object抽出",
        L"所有source handle、stage root、または固定CAB範囲が不正です");
  }
  auto status = validate_windows_adk_eula_source_identity(
      pinned_bootstrap, verified_bootstrap, pin);
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  const auto source_before = observe_native_file(
      source_handle, L"ADK EULA bootstrap 抽出前同一handle識別");
  if (!source_before || !source_before.value().regular ||
      source_before.value().reparse_point ||
      source_before.value().link_count != 1U ||
      source_before.value().byte_count != source_byte_count) {
    return failure<WindowsAdkEulaExtractionResult>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ADK EULA bootstrap 抽出前同一handle識別",
        L"所有bootstrapが固定長の通常単一リンクfile-objectではありません");
  }
  const auto source_hash_before = sha256_handle(
      source_handle,
      source_byte_count,
      L"ADK EULA bootstrap 抽出前同一handle SHA-256");
  if (!source_hash_before ||
      source_hash_before.value() != pinned_bootstrap.expected_sha256) {
    return source_hash_before
               ? failure<WindowsAdkEulaExtractionResult>(
                     clonecore::ErrorCode::verification_failed,
                     ERROR_CRC,
                     L"ADK EULA bootstrap 抽出前同一handle SHA-256",
                     L"bootstrap全体SHA-256が固定値と一致しません")
               : clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
                     source_hash_before.error());
  }

  const auto cab_path = unique_temporary_path(
      owned_stage_root, L".embedded-eula.cab");
  const auto eula_path = unique_temporary_path(
      owned_stage_root, L".embedded-eula.rtf");
  if (!cab_path || !eula_path || cab_path.value() == eula_path.value()) {
    return !cab_path
               ? clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
                     cab_path.error())
               : !eula_path
                     ? clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
                           eula_path.error())
                     : failure<WindowsAdkEulaExtractionResult>(
                           clonecore::ErrorCode::internal_error,
                           ERROR_ALREADY_EXISTS,
                           L"ADK EULA一時名",
                           L"CABとEULAの一時名が衝突しました");
  }
  OwnedTemporaryFile cabinet_file;
  OwnedTemporaryFile eula_file;
  status = cabinet_file.create_new(
      cab_path.value(), L"ADK EULA付属CAB CREATE_NEW");
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  status = copy_and_readback_container(
      source_handle, cabinet_file.handle(), pin);
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  const auto cabinet_observation = observe_native_file(
      cabinet_file.handle(), L"ADK EULA付属CAB 読戻し後識別");
  if (!cabinet_observation || !cabinet_observation.value().regular ||
      cabinet_observation.value().reparse_point ||
      cabinet_observation.value().link_count != 1U ||
      cabinet_observation.value().byte_count != pin.container_length) {
    return failure<WindowsAdkEulaExtractionResult>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA付属CAB 読戻し後識別",
        L"固定長の通常単一リンクCABではありません");
  }
  status = seek_absolute(
      cabinet_file.handle(), 0U, L"ADK EULA付属CAB header位置決め");
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  std::array<std::byte, kCabHeaderBytes> header{};
  status = read_exact(
      cabinet_file.handle(), header, L"ADK EULA付属CAB header読取り");
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  const auto parsed = inspect_windows_adk_embedded_cab_header(
      header, pin.container_length);
  if (!parsed) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        parsed.error());
  }

  std::vector<std::byte> manifest;
  status = extract_pinned_members_with_fdi(
      cabinet_file.handle(),
      parsed.value(),
      pin,
      eula_file,
      eula_path.value(),
      manifest);
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  status = validate_windows_adk_burn_eula_mapping(manifest, pin);
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  const auto member_observation = observe_native_file(
      eula_file.handle(), L"ADK EULA member 同一handle識別");
  if (!member_observation || !member_observation.value().regular ||
      member_observation.value().reparse_point ||
      member_observation.value().link_count != 1U ||
      member_observation.value().byte_count != pin.expected_byte_count) {
    return failure<WindowsAdkEulaExtractionResult>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA member 同一handle識別",
        L"EULA memberの長さ、link数、または属性が固定値と一致しません");
  }
  const auto member_hash = sha256_handle(
      eula_file.handle(),
      member_observation.value().byte_count,
      L"ADK EULA member 同一handle SHA-256");
  if (!member_hash || member_hash.value() != pin.expected_sha256) {
    return member_hash
               ? failure<WindowsAdkEulaExtractionResult>(
                     clonecore::ErrorCode::verification_failed,
                     ERROR_CRC,
                     L"ADK EULA member 同一handle SHA-256",
                     L"EULA memberのSHA-256が固定値と一致しません")
               : clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
                     member_hash.error());
  }
  auto document = read_whole_handle(
      eula_file.handle(),
      member_observation.value().byte_count,
      L"ADK EULA member 同一handle全文読取り");
  if (!document) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        document.error());
  }
  if (!contains_ascii_title(
          document.value(), pin.expected_document_title)) {
    return failure<WindowsAdkEulaExtractionResult>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK EULA member title検証",
        L"固定ADK EULA titleを全文内で確認できません");
  }
  const auto source_after = observe_native_file(
      source_handle, L"ADK EULA bootstrap 抽出後同一handle識別");
  if (!source_after ||
      !source_after.value().same_identity(source_before.value()) ||
      !source_after.value().regular || source_after.value().reparse_point ||
      source_after.value().link_count != 1U ||
      source_after.value().byte_count != source_byte_count) {
    return failure<WindowsAdkEulaExtractionResult>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ADK EULA bootstrap 抽出後同一handle識別",
        L"抽出中にbootstrapのfile-object識別または属性が変化しました");
  }
  const auto source_hash_after = sha256_handle(
      source_handle,
      source_byte_count,
      L"ADK EULA bootstrap 抽出後同一handle SHA-256");
  if (!source_hash_after ||
      source_hash_after.value() != source_hash_before.value()) {
    return source_hash_after
               ? failure<WindowsAdkEulaExtractionResult>(
                     clonecore::ErrorCode::verification_failed,
                     ERROR_CRC,
                     L"ADK EULA bootstrap 抽出後同一handle SHA-256",
                     L"抽出前後でbootstrap全体SHA-256が変化しました")
               : clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
                     source_hash_after.error());
  }

  WindowsAdkEulaExtractionObservation observation{
      .bootstrap_full_identity_verified = true,
      .attached_container_created_new = true,
      .attached_container_regular_file = true,
      .attached_container_single_link = true,
      .attached_container_reparse_point = false,
      .attached_container_bounds_verified = true,
      .attached_container_readback_equal = true,
      .cabinet_api_structure_verified = true,
      .burn_manifest_bounded = true,
      .burn_ux_mapping_verified = true,
      .member_copy_created_new = true,
      .member_copy_regular_file = true,
      .member_copy_single_link = true,
      .member_copy_reparse_point = false,
      .member_byte_count = member_observation.value().byte_count,
      .member_sha256 = member_hash.value(),
      .member_title_verified = true,
      .bounded_read_complete = true,
  };
  status = eula_file.cleanup();
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  status = cabinet_file.cleanup();
  if (!status) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        status.error());
  }
  observation.temporary_files_removed = true;
  const auto receipt = finalize_windows_adk_eula_receipt(pin, observation);
  if (!receipt) {
    return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
        receipt.error());
  }
  return clonecore::Result<WindowsAdkEulaExtractionResult>::success(
      WindowsAdkEulaExtractionResult{
          .receipt = receipt.value(),
          .rtf_document = document.take_value(),
      });
}

}  // namespace detail
}  // namespace ytec::windowsapp

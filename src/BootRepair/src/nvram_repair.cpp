#include "ytec/bootrepair/nvram_repair.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>

namespace ytec::bootrepair {
namespace {

using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::Status;

constexpr std::uint32_t kEfiVariableAttributes = 0x00000007U;
constexpr std::uint32_t kLoadOptionActive = 0x00000001U;
constexpr std::size_t kMaximumVariableBytes = 128U * 1024U;
constexpr std::size_t kMaximumBootOrderEntries = 4096U;
constexpr std::uint16_t kMaximumCandidateProbeCount = 4096U;
constexpr std::wstring_view kWindowsBootManagerPath =
    L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
constexpr std::wstring_view kEfiSystemPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr wchar_t kEfiGlobalVariableGuid[] =
    L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

clonecore::Error nvram_error(
    const ErrorCode code,
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

bool equals_case_insensitive(
    const std::wstring& left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      _wcsicmp(left.c_str(), std::wstring(right).c_str()) == 0;
}

bool is_allowed_variable_name(const std::wstring& name) noexcept {
  if (name == L"BootOrder") {
    return true;
  }
  if (name.size() != 8U || name.compare(0U, 4U, L"Boot") != 0) {
    return false;
  }
  return std::all_of(name.begin() + 4, name.end(), [](const wchar_t value) {
    return (value >= L'0' && value <= L'9') ||
        (value >= L'A' && value <= L'F');
  });
}

template <typename T>
void append_little_endian(std::vector<std::byte>& output, const T value) {
  static_assert(std::is_unsigned_v<T>);
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    output.push_back(static_cast<std::byte>(
        (value >> (index * 8U)) & static_cast<T>(0xFFU)));
  }
}

template <typename T>
bool read_little_endian(
    const std::span<const std::byte> input,
    const std::size_t offset,
    T& value) noexcept {
  static_assert(std::is_unsigned_v<T>);
  if (offset > input.size() || sizeof(T) > input.size() - offset) {
    return false;
  }
  value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    value |= static_cast<T>(std::to_integer<unsigned char>(input[offset + index]))
        << (index * 8U);
  }
  return true;
}

Result<std::array<std::byte, 16>> parse_partition_guid(
    const std::wstring& identifier) {
  GUID guid{};
  if (identifier.empty() || CLSIDFromString(identifier.c_str(), &guid) != S_OK) {
    return Result<std::array<std::byte, 16>>::failure(nvram_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"UEFI NVRAM対象ESP GUID",
        L"レビュー済みGPTパーティションGUIDを正規形式で解釈できません"));
  }
  std::array<std::byte, 16> bytes{};
  std::memcpy(bytes.data(), &guid, bytes.size());
  return Result<std::array<std::byte, 16>>::success(bytes);
}

Status validate_request(const CurrentPcNvramRepairRequest& request) {
  if (!request.explicitly_for_current_pc ||
      !request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK") {
    return Status::failure(nvram_error(
        ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"現在PCのUEFI NVRAM二段階確認",
        L"「このPCで使用する」の明示選択と大文字OK入力が必要です"));
  }
  if (request.expected_disk.disk_number ==
          (std::numeric_limits<std::uint32_t>::max)() ||
      request.expected_disk.model.empty() || request.expected_disk.size_bytes == 0U ||
      request.expected_disk.logical_sector_size != request.logical_sector_size ||
      request.logical_sector_size != 512U ||
      request.expected_esp.style != diskmodel::PartitionStyle::gpt ||
      !equals_case_insensitive(request.expected_esp.type, kEfiSystemPartitionType) ||
      request.expected_esp.number == 0U || request.expected_esp.offset_bytes == 0U ||
      request.expected_esp.size_bytes == 0U ||
      request.expected_esp.offset_bytes % request.logical_sector_size != 0U ||
      request.expected_esp.size_bytes % request.logical_sector_size != 0U ||
      request.expected_esp.offset_bytes > request.expected_disk.size_bytes ||
      request.expected_esp.size_bytes >
          request.expected_disk.size_bytes - request.expected_esp.offset_bytes) {
    return Status::failure(nvram_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"現在PCのUEFI NVRAM対象構成",
        L"完全に識別した512バイト論理セクターのGPT ESPだけを対象にできます"));
  }
  const auto guid = parse_partition_guid(request.expected_esp.identifier);
  if (!guid) {
    return Status::failure(guid.error());
  }
  return clonecore::success_status();
}

std::wstring boot_option_name(const std::uint16_t number) {
  std::wostringstream stream;
  stream << L"Boot" << std::uppercase << std::hex << std::setw(4)
         << std::setfill(L'0') << number;
  return stream.str();
}

Result<std::vector<std::uint16_t>> parse_boot_order(
    const std::optional<FirmwareVariableValue>& value) {
  if (!value.has_value()) {
    return Result<std::vector<std::uint16_t>>::success({});
  }
  if (value->attributes != kEfiVariableAttributes || value->bytes.empty() ||
      value->bytes.size() % sizeof(std::uint16_t) != 0U ||
      value->bytes.size() / sizeof(std::uint16_t) > kMaximumBootOrderEntries) {
    return Result<std::vector<std::uint16_t>>::failure(nvram_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"UEFI BootOrder形式",
        L"通常のNV/BS/RT属性を持つ有界な16ビットBootOrderではありません"));
  }
  std::vector<std::uint16_t> order;
  order.reserve(value->bytes.size() / sizeof(std::uint16_t));
  for (std::size_t offset = 0; offset < value->bytes.size(); offset += 2U) {
    std::uint16_t number = 0;
    (void)read_little_endian<std::uint16_t>(value->bytes, offset, number);
    if (std::find(order.begin(), order.end(), number) != order.end()) {
      return Result<std::vector<std::uint16_t>>::failure(nvram_error(
          ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"UEFI BootOrder重複",
          L"同じBoot####番号が複数含まれる曖昧なBootOrderは変更しません"));
    }
    order.push_back(number);
  }
  return Result<std::vector<std::uint16_t>>::success(std::move(order));
}

FirmwareVariableValue encode_boot_order(
    const std::vector<std::uint16_t>& order) {
  FirmwareVariableValue value{.attributes = kEfiVariableAttributes};
  value.bytes.reserve(order.size() * sizeof(std::uint16_t));
  for (const auto number : order) {
    append_little_endian(value.bytes, number);
  }
  return value;
}

bool same_variable(
    const std::optional<FirmwareVariableValue>& left,
    const std::optional<FirmwareVariableValue>& right) noexcept {
  return left.has_value() == right.has_value() &&
      (!left.has_value() ||
       (left->attributes == right->attributes && left->bytes == right->bytes));
}

Status restore_variable(
    ICurrentPcNvramRepairPlatform& platform,
    const std::wstring& name,
    const std::optional<FirmwareVariableValue>& current,
    const std::optional<FirmwareVariableValue>& original) {
  return platform.replace_efi_global_variable_if_exact(name, current, original);
}

clonecore::Error with_rollback_status(
    clonecore::Error error,
    const CurrentPcNvramRepairReport& report) {
  if (!report.rollback_attempted) {
    return error;
  }
  error.message += report.rollback_succeeded
      ? L"。この処理が書き込んだ値は条件付き読戻しで変更前へ復元済みです"
      : L"。変更前状態への復元を完全確認できません。NVRAMは部分修復として扱ってください";
  return error;
}

class ScopedSystemEnvironmentPrivilege final {
 public:
  ScopedSystemEnvironmentPrivilege() {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
            &token_raw)) {
      error_ = GetLastError();
      return;
    }
    token_.reset(token_raw);
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &luid)) {
      error_ = GetLastError();
      return;
    }
    TOKEN_PRIVILEGES requested{};
    requested.PrivilegeCount = 1;
    requested.Privileges[0].Luid = luid;
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    DWORD previous_size = sizeof(previous_);
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(
            token_.get(), FALSE, &requested, sizeof(previous_), &previous_,
            &previous_size) ||
        GetLastError() != ERROR_SUCCESS) {
      error_ = GetLastError();
      return;
    }
    previous_size_ = previous_size;
    enabled_ = true;
  }

  ~ScopedSystemEnvironmentPrivilege() {
    if (enabled_ && token_) {
      (void)AdjustTokenPrivileges(
          token_.get(), FALSE, &previous_, previous_size_, nullptr, nullptr);
    }
  }

  ScopedSystemEnvironmentPrivilege(const ScopedSystemEnvironmentPrivilege&) = delete;
  ScopedSystemEnvironmentPrivilege& operator=(
      const ScopedSystemEnvironmentPrivilege&) = delete;

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  [[nodiscard]] DWORD error() const noexcept { return error_; }

 private:
  clonecore::UniqueHandle token_;
  TOKEN_PRIVILEGES previous_{};
  DWORD previous_size_{};
  DWORD error_{ERROR_PRIVILEGE_NOT_HELD};
  bool enabled_{};
};

class WindowsCurrentPcNvramRepairPlatform final
    : public ICurrentPcNvramRepairPlatform {
 public:
  explicit WindowsCurrentPcNvramRepairPlatform(
      CurrentPcNvramTargetRevalidator revalidator)
      : revalidator_(std::move(revalidator)) {}

  Status revalidate_target(
      const clonecore::StableDiskIdentity& expected_disk,
      const diskmodel::PartitionInfo& expected_esp) override {
    if (!revalidator_) {
      return Status::failure(nvram_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"UEFI NVRAM対象再識別callback",
          L"製品のfresh disk/layout再識別callbackがありません"));
    }
    return revalidator_(expected_disk, expected_esp);
  }

  Result<std::optional<FirmwareVariableValue>> read_efi_global_variable(
      const std::wstring& name) override {
    if (!is_allowed_variable_name(name)) {
      return Result<std::optional<FirmwareVariableValue>>::failure(nvram_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          L"UEFI firmware variable名",
          L"BootOrderまたは大文字16進4桁のBoot####だけを参照できます"));
    }
    ScopedSystemEnvironmentPrivilege privilege;
    if (!privilege.enabled()) {
      return Result<std::optional<FirmwareVariableValue>>::failure(
          clonecore::make_win32_error(
              ErrorCode::access_denied,
              L"UEFI firmware variable読取り権限",
              privilege.error()));
    }
    return read_efi_global_variable_with_privilege(name);
  }

  Status replace_efi_global_variable_if_exact(
      const std::wstring& name,
      const std::optional<FirmwareVariableValue>& expected,
      const std::optional<FirmwareVariableValue>& replacement) override {
    if (!is_allowed_variable_name(name) ||
        (!expected.has_value() && !replacement.has_value()) ||
        (expected.has_value() && !valid_variable_value(expected.value())) ||
        (replacement.has_value() && !valid_variable_value(replacement.value()))) {
      return Status::failure(nvram_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"UEFI firmware variable条件付き更新要求",
          L"通常属性を持つ1～128KiBのBootOrder/Boot####だけを更新できます"));
    }
    ScopedSystemEnvironmentPrivilege privilege;
    if (!privilege.enabled()) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::access_denied,
          L"UEFI firmware variable更新権限",
          privilege.error()));
    }
    static std::mutex mutation_mutex;
    const std::lock_guard lock(mutation_mutex);
    auto current = read_efi_global_variable_with_privilege(name);
    if (!current) {
      return Status::failure(current.error());
    }
    if (!same_variable(current.value(), expected)) {
      return Status::failure(nvram_error(
          ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"UEFI firmware variable条件付き更新",
          L"直前確認後に対象variableが変化したため更新しません"));
    }
    if (same_variable(expected, replacement)) {
      return clonecore::success_status();
    }
    const void* data = replacement.has_value()
        ? static_cast<const void*>(replacement->bytes.data())
        : nullptr;
    const DWORD length = replacement.has_value()
        ? static_cast<DWORD>(replacement->bytes.size())
        : 0U;
    const DWORD attributes = replacement.has_value()
        ? replacement->attributes
        : expected->attributes;
    if (!SetFirmwareEnvironmentVariableExW(
            name.c_str(), kEfiGlobalVariableGuid, const_cast<void*>(data),
            length, attributes)) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          replacement.has_value() ? L"UEFI firmware variable書込み"
                                  : L"UEFI firmware variable削除",
          GetLastError()));
    }
    return clonecore::success_status();
  }

 private:
  static bool valid_variable_value(const FirmwareVariableValue& value) noexcept {
    return value.attributes == kEfiVariableAttributes && !value.bytes.empty() &&
        value.bytes.size() <= kMaximumVariableBytes;
  }

  Result<std::optional<FirmwareVariableValue>>
  read_efi_global_variable_with_privilege(const std::wstring& name) {
    std::vector<std::byte> bytes(kMaximumVariableBytes);
    DWORD attributes = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetFirmwareEnvironmentVariableExW(
        name.c_str(), kEfiGlobalVariableGuid, bytes.data(),
        static_cast<DWORD>(bytes.size()), &attributes);
    if (length == 0U) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_ENVVAR_NOT_FOUND || native_code == ERROR_NOT_FOUND) {
        return Result<std::optional<FirmwareVariableValue>>::success(std::nullopt);
      }
      return Result<std::optional<FirmwareVariableValue>>::failure(
          clonecore::make_win32_error(
              ErrorCode::io_failed,
              L"UEFI firmware variable読取り",
              native_code));
    }
    if (length > bytes.size()) {
      return Result<std::optional<FirmwareVariableValue>>::failure(nvram_error(
          ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"UEFI firmware variable上限",
          L"firmware variableが128KiBの安全上限を超えました"));
    }
    bytes.resize(length);
    return Result<std::optional<FirmwareVariableValue>>::success(
        FirmwareVariableValue{.bytes = std::move(bytes), .attributes = attributes});
  }
  CurrentPcNvramTargetRevalidator revalidator_;
};

}  // namespace

Result<std::vector<std::byte>> build_windows_boot_manager_load_option(
    const diskmodel::PartitionInfo& expected_esp,
    const std::uint32_t logical_sector_size) {
  if (expected_esp.style != diskmodel::PartitionStyle::gpt ||
      expected_esp.number == 0U || expected_esp.offset_bytes == 0U ||
      expected_esp.size_bytes == 0U || logical_sector_size != 512U ||
      expected_esp.offset_bytes % logical_sector_size != 0U ||
      expected_esp.size_bytes % logical_sector_size != 0U) {
    return Result<std::vector<std::byte>>::failure(nvram_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows Boot Manager device path",
        L"512バイト論理セクターのGPT ESPだけを表現できます"));
  }
  auto signature = parse_partition_guid(expected_esp.identifier);
  if (!signature) {
    return Result<std::vector<std::byte>>::failure(signature.error());
  }

  std::vector<std::byte> device_path;
  device_path.reserve(160U);
  // MEDIA_DEVICE_PATH / MEDIA_HARDDRIVE_DP / length 42.
  device_path.push_back(std::byte{0x04});
  device_path.push_back(std::byte{0x01});
  append_little_endian<std::uint16_t>(device_path, 42U);
  append_little_endian<std::uint32_t>(device_path, expected_esp.number);
  append_little_endian<std::uint64_t>(
      device_path, expected_esp.offset_bytes / logical_sector_size);
  append_little_endian<std::uint64_t>(
      device_path, expected_esp.size_bytes / logical_sector_size);
  device_path.insert(
      device_path.end(), signature.value().begin(), signature.value().end());
  device_path.push_back(std::byte{0x02});  // GPT partition format.
  device_path.push_back(std::byte{0x02});  // GUID signature.

  const std::size_t file_path_node_bytes =
      4U + (kWindowsBootManagerPath.size() + 1U) * sizeof(wchar_t);
  if (file_path_node_bytes > (std::numeric_limits<std::uint16_t>::max)()) {
    return Result<std::vector<std::byte>>::failure(nvram_error(
        ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"Windows Boot Manager file path node",
        L"EFI file pathが16ビットnode長を超えました"));
  }
  device_path.push_back(std::byte{0x04});
  device_path.push_back(std::byte{0x04});
  append_little_endian<std::uint16_t>(
      device_path, static_cast<std::uint16_t>(file_path_node_bytes));
  for (const wchar_t character : kWindowsBootManagerPath) {
    append_little_endian<std::uint16_t>(
        device_path, static_cast<std::uint16_t>(character));
  }
  append_little_endian<std::uint16_t>(device_path, 0U);
  device_path.insert(
      device_path.end(),
      {std::byte{0x7F}, std::byte{0xFF}, std::byte{0x04}, std::byte{0x00}});

  constexpr std::wstring_view kDescription = L"Windows Boot Manager";
  if (device_path.size() > (std::numeric_limits<std::uint16_t>::max)()) {
    return Result<std::vector<std::byte>>::failure(nvram_error(
        ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"Windows Boot Manager device path長",
        L"EFI device pathが16ビット上限を超えました"));
  }
  std::vector<std::byte> option;
  option.reserve(6U + (kDescription.size() + 1U) * 2U + device_path.size());
  append_little_endian<std::uint32_t>(option, kLoadOptionActive);
  append_little_endian<std::uint16_t>(
      option, static_cast<std::uint16_t>(device_path.size()));
  for (const wchar_t character : kDescription) {
    append_little_endian<std::uint16_t>(
        option, static_cast<std::uint16_t>(character));
  }
  append_little_endian<std::uint16_t>(option, 0U);
  option.insert(option.end(), device_path.begin(), device_path.end());
  return Result<std::vector<std::byte>>::success(std::move(option));
}

Result<bool> windows_boot_manager_load_option_matches(
    const std::span<const std::byte> load_option,
    const diskmodel::PartitionInfo& expected_esp,
    const std::uint32_t logical_sector_size) {
  if (load_option.size() < 6U || load_option.size() > kMaximumVariableBytes) {
    return Result<bool>::failure(nvram_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"UEFI Boot#### load option長",
        L"load optionが安全な長さではありません"));
  }
  std::uint32_t attributes = 0;
  std::uint16_t file_path_length = 0;
  if (!read_little_endian(load_option, 0U, attributes) ||
      !read_little_endian(load_option, 4U, file_path_length)) {
    return Result<bool>::failure(nvram_error(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"UEFI Boot#### header", L"load option headerが途中で終端しています"));
  }
  (void)attributes;
  std::size_t description_end = 6U;
  std::size_t description_characters = 0U;
  while (description_end + 2U <= load_option.size()) {
    std::uint16_t character = 0;
    (void)read_little_endian(load_option, description_end, character);
    description_end += 2U;
    if (character == 0U) {
      break;
    }
    if (++description_characters > 256U) {
      return Result<bool>::failure(nvram_error(
          ErrorCode::invalid_data, ERROR_BUFFER_OVERFLOW,
          L"UEFI Boot#### description", L"descriptionが256文字を超えました"));
    }
  }
  if (description_end > load_option.size() ||
      file_path_length > load_option.size() - description_end ||
      file_path_length < 4U) {
    return Result<bool>::failure(nvram_error(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"UEFI Boot#### device path", L"device path範囲がload option外です"));
  }
  const auto path = load_option.subspan(description_end, file_path_length);
  if (path[0] != std::byte{0x04} || path[1] != std::byte{0x01}) {
    return Result<bool>::success(false);
  }
  if (path.size() < 50U) {
    return Result<bool>::failure(nvram_error(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"UEFI Boot#### hard-drive device path",
        L"GPT hard-drive device pathが途中で終端しています"));
  }
  std::uint16_t hard_drive_length = 0;
  std::uint32_t partition_number = 0;
  std::uint64_t starting_lba = 0;
  std::uint64_t size_lba = 0;
  if (!read_little_endian(path, 2U, hard_drive_length) ||
      hard_drive_length != 42U ||
      !read_little_endian(path, 4U, partition_number) ||
      !read_little_endian(path, 8U, starting_lba) ||
      !read_little_endian(path, 16U, size_lba) ||
      path[40] != std::byte{0x02} || path[41] != std::byte{0x02}) {
    return Result<bool>::success(false);
  }
  auto signature = parse_partition_guid(expected_esp.identifier);
  if (!signature) {
    return Result<bool>::failure(signature.error());
  }
  if (!std::equal(signature.value().begin(), signature.value().end(), path.begin() + 24U) ||
      logical_sector_size != 512U || partition_number != expected_esp.number ||
      starting_lba != expected_esp.offset_bytes / logical_sector_size ||
      size_lba != expected_esp.size_bytes / logical_sector_size) {
    return Result<bool>::success(false);
  }
  const std::size_t file_node_offset = 42U;
  if (file_node_offset + 4U > path.size() ||
      path[file_node_offset] != std::byte{0x04} ||
      path[file_node_offset + 1U] != std::byte{0x04}) {
    return Result<bool>::success(false);
  }
  std::uint16_t file_node_length = 0;
  if (!read_little_endian(path, file_node_offset + 2U, file_node_length) ||
      file_node_length < 6U || file_node_offset + file_node_length + 4U != path.size()) {
    return Result<bool>::failure(nvram_error(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"UEFI Boot#### file path node", L"file path nodeの長さが不正です"));
  }
  std::wstring file_path;
  for (std::size_t offset = file_node_offset + 4U;
       offset + 2U <= file_node_offset + file_node_length; offset += 2U) {
    std::uint16_t character = 0;
    (void)read_little_endian(path, offset, character);
    if (character == 0U) {
      break;
    }
    file_path.push_back(static_cast<wchar_t>(character));
  }
  const std::size_t end = file_node_offset + file_node_length;
  if (end + 4U != path.size() || path[end] != std::byte{0x7F} ||
      path[end + 1U] != std::byte{0xFF} || path[end + 2U] != std::byte{0x04} ||
      path[end + 3U] != std::byte{0x00}) {
    return Result<bool>::success(false);
  }
  return Result<bool>::success(
      _wcsicmp(file_path.c_str(), std::wstring(kWindowsBootManagerPath).c_str()) == 0);
}

Result<CurrentPcNvramRepairReport> execute_current_pc_nvram_repair(
    const CurrentPcNvramRepairRequest& request,
    ICurrentPcNvramRepairPlatform& platform) {
  const Status valid = validate_request(request);
  if (!valid) {
    return Result<CurrentPcNvramRepairReport>::failure(valid.error());
  }
  const Status initial_target =
      platform.revalidate_target(request.expected_disk, request.expected_esp);
  if (!initial_target) {
    return Result<CurrentPcNvramRepairReport>::failure(initial_target.error());
  }
  auto original_order = platform.read_efi_global_variable(L"BootOrder");
  if (!original_order) {
    return Result<CurrentPcNvramRepairReport>::failure(original_order.error());
  }
  auto order = parse_boot_order(original_order.value());
  if (!order) {
    return Result<CurrentPcNvramRepairReport>::failure(order.error());
  }

  std::optional<std::uint16_t> matching_number;
  std::optional<FirmwareVariableValue> matching_value;
  for (const auto number : order.value()) {
    auto value = platform.read_efi_global_variable(boot_option_name(number));
    if (!value) {
      return Result<CurrentPcNvramRepairReport>::failure(value.error());
    }
    if (!value.value().has_value()) {
      return Result<CurrentPcNvramRepairReport>::failure(nvram_error(
          ErrorCode::invalid_data, ERROR_NOT_FOUND,
          L"UEFI BootOrder参照", L"BootOrderが存在しないBoot####を参照しています"));
    }
    if (value.value()->attributes != kEfiVariableAttributes) {
      return Result<CurrentPcNvramRepairReport>::failure(nvram_error(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"UEFI Boot####属性", L"通常のNV/BS/RT属性以外のboot optionは変更しません"));
    }
    auto matches = windows_boot_manager_load_option_matches(
        value.value()->bytes, request.expected_esp, request.logical_sector_size);
    if (!matches) {
      return Result<CurrentPcNvramRepairReport>::failure(matches.error());
    }
    if (matches.value()) {
      if (matching_number.has_value()) {
        return Result<CurrentPcNvramRepairReport>::failure(nvram_error(
            ErrorCode::invalid_data, ERROR_DUP_NAME,
            L"UEFI Windows Boot Manager重複",
            L"同じ対象ESPを指すboot optionが複数あるため自動修復しません"));
      }
      matching_number = number;
      matching_value = value.value().value();
    }
  }

  std::optional<std::uint16_t> first_absent_number;
  for (std::uint32_t probe = 0U; probe < kMaximumCandidateProbeCount; ++probe) {
    const auto number = static_cast<std::uint16_t>(probe);
    if (std::find(order.value().begin(), order.value().end(), number) !=
        order.value().end()) {
      continue;
    }
    auto value = platform.read_efi_global_variable(boot_option_name(number));
    if (!value) {
      return Result<CurrentPcNvramRepairReport>::failure(value.error());
    }
    if (!value.value().has_value()) {
      if (!first_absent_number.has_value()) {
        first_absent_number = number;
      }
      continue;
    }
    if (value.value()->attributes != kEfiVariableAttributes) {
      continue;
    }
    auto matches = windows_boot_manager_load_option_matches(
        value.value()->bytes, request.expected_esp, request.logical_sector_size);
    if (!matches) {
      return Result<CurrentPcNvramRepairReport>::failure(matches.error());
    }
    if (matches.value()) {
      if (matching_number.has_value()) {
        return Result<CurrentPcNvramRepairReport>::failure(nvram_error(
            ErrorCode::invalid_data, ERROR_DUP_NAME,
            L"UEFI Windows Boot Manager重複",
            L"BootOrder外を含め同じ対象ESPを指すboot optionが複数あります"));
      }
      matching_number = number;
      matching_value = value.value().value();
    }
  }

  CurrentPcNvramRepairReport report{};
  if (matching_number.has_value()) {
    report.boot_option_number = matching_number.value();
    std::uint32_t attributes = 0;
    (void)read_little_endian(matching_value->bytes, 0U, attributes);
    const bool listed =
        std::find(order.value().begin(), order.value().end(),
                  matching_number.value()) != order.value().end();
    if ((attributes & kLoadOptionActive) != 0U && listed) {
      const Status final_target =
          platform.revalidate_target(request.expected_disk, request.expected_esp);
      if (!final_target) {
        return Result<CurrentPcNvramRepairReport>::failure(final_target.error());
      }
      report.already_valid = true;
      report.prior_boot_order_preserved = true;
      report.exact_target_verified = true;
      return Result<CurrentPcNvramRepairReport>::success(report);
    }
  }

  std::optional<std::uint16_t> candidate_number = matching_number.has_value()
      ? matching_number
      : first_absent_number;
  std::optional<FirmwareVariableValue> original_option = matching_value;
  if (!candidate_number.has_value()) {
    return Result<CurrentPcNvramRepairReport>::failure(nvram_error(
        ErrorCode::unsupported_layout, ERROR_TOO_MANY_NAMES,
        L"UEFI Boot####空き番号", L"先頭4096個に安全な空きboot option番号がありません"));
  }
  report.boot_option_number = candidate_number.value();
  const std::wstring option_name = boot_option_name(candidate_number.value());

  FirmwareVariableValue desired_option{.attributes = kEfiVariableAttributes};
  if (original_option.has_value()) {
    desired_option = original_option.value();
    std::uint32_t attributes = 0;
    (void)read_little_endian(desired_option.bytes, 0U, attributes);
    attributes |= kLoadOptionActive;
    for (std::size_t index = 0; index < 4U; ++index) {
      desired_option.bytes[index] = static_cast<std::byte>(
          (attributes >> (index * 8U)) & 0xFFU);
    }
  } else {
    auto built = build_windows_boot_manager_load_option(
        request.expected_esp, request.logical_sector_size);
    if (!built) {
      return Result<CurrentPcNvramRepairReport>::failure(built.error());
    }
    desired_option.bytes = built.take_value();
  }

  auto current_order = platform.read_efi_global_variable(L"BootOrder");
  auto current_option = platform.read_efi_global_variable(option_name);
  if (!current_order || !current_option) {
    return Result<CurrentPcNvramRepairReport>::failure(
        !current_order ? current_order.error() : current_option.error());
  }
  if (!same_variable(current_order.value(), original_order.value()) ||
      !same_variable(current_option.value(), original_option)) {
    return Result<CurrentPcNvramRepairReport>::failure(nvram_error(
        ErrorCode::identity_mismatch, ERROR_DEVICE_NOT_CONNECTED,
        L"UEFI firmware variable実行直前再照合",
        L"レビュー後にBootOrderまたは対象Boot####が変化しました"));
  }
  const Status before_option =
      platform.revalidate_target(request.expected_disk, request.expected_esp);
  if (!before_option) {
    return Result<CurrentPcNvramRepairReport>::failure(before_option.error());
  }
  const std::optional<FirmwareVariableValue> desired_option_value = desired_option;
  const bool option_needs_write =
      !same_variable(original_option, desired_option_value);
  if (option_needs_write) {
    const Status option_written = platform.replace_efi_global_variable_if_exact(
        option_name, original_option, desired_option_value);
    if (!option_written) {
      return Result<CurrentPcNvramRepairReport>::failure(option_written.error());
    }
    report.boot_option_written = true;
    auto verified_option = platform.read_efi_global_variable(option_name);
    if (!verified_option ||
        !same_variable(verified_option.value(), desired_option_value)) {
      report.rollback_attempted = true;
      report.rollback_succeeded = static_cast<bool>(restore_variable(
          platform, option_name, desired_option_value, original_option));
      return Result<CurrentPcNvramRepairReport>::failure(with_rollback_status(
          verified_option ? nvram_error(
                                ErrorCode::verification_failed,
                                ERROR_CRC,
                                L"UEFI Boot####読戻し",
                                L"書込み後のboot optionが完全一致しません")
                          : verified_option.error(),
          report));
    }
  }

  std::optional<FirmwareVariableValue> desired_order_value;
  if (std::find(order.value().begin(), order.value().end(),
                candidate_number.value()) == order.value().end()) {
    auto new_order = order.value();
    new_order.push_back(candidate_number.value());
    const auto desired_order = encode_boot_order(new_order);
    desired_order_value = desired_order;
    const Status before_order =
        platform.revalidate_target(request.expected_disk, request.expected_esp);
    if (!before_order) {
      report.rollback_attempted = option_needs_write;
      report.rollback_succeeded = !option_needs_write ||
          static_cast<bool>(restore_variable(
              platform, option_name, desired_option_value, original_option));
      return Result<CurrentPcNvramRepairReport>::failure(
          with_rollback_status(before_order.error(), report));
    }
    const Status order_written = platform.replace_efi_global_variable_if_exact(
        L"BootOrder", original_order.value(), desired_order_value);
    if (!order_written) {
      report.rollback_attempted = option_needs_write;
      report.rollback_succeeded = !option_needs_write ||
          static_cast<bool>(restore_variable(
              platform, option_name, desired_option_value, original_option));
      return Result<CurrentPcNvramRepairReport>::failure(
          with_rollback_status(order_written.error(), report));
    }
    report.boot_order_written = true;
    auto verified_order = platform.read_efi_global_variable(L"BootOrder");
    if (!verified_order || !same_variable(verified_order.value(), desired_order)) {
      report.rollback_attempted = true;
      const Status order_rollback = restore_variable(
          platform, L"BootOrder", desired_order_value, original_order.value());
      const bool order_rollback_succeeded = static_cast<bool>(order_rollback);
      const bool option_rollback_succeeded = !option_needs_write ||
          static_cast<bool>(restore_variable(
              platform, option_name, desired_option_value, original_option));
      report.rollback_succeeded =
          order_rollback_succeeded && option_rollback_succeeded;
      return Result<CurrentPcNvramRepairReport>::failure(with_rollback_status(
          verified_order ? nvram_error(
                               ErrorCode::verification_failed,
                               ERROR_CRC,
                               L"UEFI BootOrder読戻し",
                               L"既存順序を保持して末尾追加した結果を確認できません")
                         : verified_order.error(),
          report));
    }
  }

  const auto rollback_all = [&]() {
    report.rollback_attempted = report.boot_order_written || option_needs_write;
    const bool order_ok = !report.boot_order_written ||
        static_cast<bool>(restore_variable(
            platform, L"BootOrder", desired_order_value, original_order.value()));
    const bool option_ok = !option_needs_write ||
        static_cast<bool>(restore_variable(
            platform, option_name, desired_option_value, original_option));
    report.rollback_succeeded = order_ok && option_ok;
  };

  auto final_option = platform.read_efi_global_variable(option_name);
  auto final_order = platform.read_efi_global_variable(L"BootOrder");
  if (!final_option || !final_order || !final_option.value().has_value() ||
      !final_order.value().has_value()) {
    rollback_all();
    return Result<CurrentPcNvramRepairReport>::failure(with_rollback_status(
        nvram_error(
        ErrorCode::verification_failed, ERROR_CRC,
        L"UEFI NVRAM最終再検証", L"Boot####またはBootOrderを最終読取りできません"),
        report));
  }
  auto exact = windows_boot_manager_load_option_matches(
      final_option.value()->bytes, request.expected_esp,
      request.logical_sector_size);
  auto final_numbers = parse_boot_order(final_order.value());
  const auto& expected_final_order = desired_order_value.has_value()
      ? desired_order_value
      : original_order.value();
  if (!exact || !exact.value() || !final_numbers ||
      !same_variable(final_option.value(), desired_option_value) ||
      !same_variable(final_order.value(), expected_final_order) ||
      std::find(final_numbers.value().begin(), final_numbers.value().end(),
                candidate_number.value()) == final_numbers.value().end() ||
      final_numbers.value().size() != order.value().size() +
          (std::find(order.value().begin(), order.value().end(),
                     candidate_number.value()) == order.value().end() ? 1U : 0U) ||
      !std::equal(order.value().begin(), order.value().end(),
                  final_numbers.value().begin())) {
    rollback_all();
    return Result<CurrentPcNvramRepairReport>::failure(with_rollback_status(
        nvram_error(
        ErrorCode::verification_failed, ERROR_CRC,
        L"UEFI NVRAM最終内容",
        L"対象ESPまたは既存BootOrder順序を完全確認できません"),
        report));
  }
  const Status final_target =
      platform.revalidate_target(request.expected_disk, request.expected_esp);
  if (!final_target) {
    rollback_all();
    return Result<CurrentPcNvramRepairReport>::failure(
        with_rollback_status(final_target.error(), report));
  }
  report.prior_boot_order_preserved = true;
  report.exact_target_verified = true;
  return Result<CurrentPcNvramRepairReport>::success(report);
}

std::unique_ptr<ICurrentPcNvramRepairPlatform>
make_windows_current_pc_nvram_repair_platform(
    CurrentPcNvramTargetRevalidator target_revalidator) {
  return std::make_unique<WindowsCurrentPcNvramRepairPlatform>(
      std::move(target_revalidator));
}

}  // namespace ytec::bootrepair

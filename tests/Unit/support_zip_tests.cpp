#include "ytec/windowsapp/support_zip.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
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

struct TestFailure final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

bool file_exists(const std::wstring& path) {
  return GetFileAttributesW(extended_path(path).c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

void write_bytes(
    const std::wstring& path,
    const std::span<const std::byte> bytes) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0U,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(file.valid(), "Synthetic file must open for replacement");
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>((std::min)(
        bytes.size() - consumed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written{};
    check(WriteFile(
              file.get(),
              bytes.data() + consumed,
              amount,
              &written,
              nullptr) != FALSE &&
              written != 0U,
          "Synthetic file write must complete");
    consumed += static_cast<std::size_t>(written);
  }
  check(FlushFileBuffers(file.get()) != FALSE,
        "Synthetic file must flush");
}

std::vector<std::byte> read_bytes(const std::wstring& path) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(file.valid(), "Synthetic file must open for reading");
  LARGE_INTEGER size{};
  check(GetFileSizeEx(file.get(), &size) != FALSE && size.QuadPart >= 0,
        "Synthetic file size must be available");
  check(static_cast<std::uint64_t>(size.QuadPart) <= 4U * 1024U * 1024U,
        "Test helper must not allocate an unexpectedly large file");
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    DWORD read{};
    const DWORD amount = static_cast<DWORD>(bytes.size() - consumed);
    check(ReadFile(
              file.get(),
              bytes.data() + consumed,
              amount,
              &read,
              nullptr) != FALSE &&
              read != 0U,
          "Synthetic file read must complete");
    consumed += static_cast<std::size_t>(read);
  }
  return bytes;
}

std::vector<std::byte> utf8_bom_log(const std::string_view body) {
  std::vector<std::byte> bytes{
      std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};
  bytes.reserve(bytes.size() + body.size());
  for (const char value : body) {
    bytes.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(value)));
  }
  return bytes;
}

bool contains_ascii(
    const std::span<const std::byte> bytes,
    const std::string_view needle) {
  return std::search(
             bytes.begin(),
             bytes.end(),
             needle.begin(),
             needle.end(),
             [](const std::byte left, const char right) {
               return left == static_cast<std::byte>(
                                  static_cast<unsigned char>(right));
             }) != bytes.end();
}

bool contains_u32(
    const std::span<const std::byte> bytes,
    const std::uint32_t value) {
  const std::array<std::byte, 4U> encoded{
      static_cast<std::byte>(value & 0xFFU),
      static_cast<std::byte>((value >> 8U) & 0xFFU),
      static_cast<std::byte>((value >> 16U) & 0xFFU),
      static_cast<std::byte>((value >> 24U) & 0xFFU),
  };
  return std::search(
             bytes.begin(), bytes.end(), encoded.begin(), encoded.end()) !=
         bytes.end();
}

bool same_bytes(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

std::string utf8(const std::wstring_view value) {
  check(value.size() <=
            static_cast<std::size_t>((std::numeric_limits<int>::max)()),
        "Diagnostic text must fit the Windows UTF-8 converter");
  if (value.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  check(length > 0, "Diagnostic text must convert to UTF-8");
  std::string bytes(static_cast<std::size_t>(length), '\0');
  check(WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            bytes.data(),
            length,
            nullptr,
            nullptr) == length,
        "Diagnostic UTF-8 conversion must complete");
  return bytes;
}

std::wstring module_directory() {
  std::vector<wchar_t> buffer(32U * 1024U, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  check(length != 0U && static_cast<std::size_t>(length) < buffer.size(),
        "Test module path must be available");
  std::wstring path(buffer.data(), length);
  const std::size_t separator = path.find_last_of(L'\\');
  check(separator != std::wstring::npos,
        "Test module path must have a parent");
  return path.substr(0U, separator);
}

class TemporaryTree final {
 public:
  TemporaryTree() {
    static unsigned int sequence{};
    const std::wstring parent = module_directory();
    for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
      ++sequence;
      const std::wstring candidate = parent + L"\\support-zip-test-" +
          std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(sequence);
      if (CreateDirectoryW(extended_path(candidate).c_str(), nullptr) !=
          FALSE) {
        root_ = candidate;
        break;
      }
      check(GetLastError() == ERROR_ALREADY_EXISTS,
            "Synthetic root creation must only collide with a prior fixture");
    }
    check(!root_.empty(), "A bounded synthetic root must be created");
    application_ = root_ + L"\\app";
    data_ = application_ + L"\\data";
    logs_ = data_ + L"\\logs";
    output_ = root_ + L"\\output";
    executable_ = application_ + L"\\TsumugiDrive.exe";
    final_ = output_ + L"\\support.zip";
    check(CreateDirectoryW(extended_path(application_).c_str(), nullptr) !=
                  FALSE &&
              CreateDirectoryW(extended_path(data_).c_str(), nullptr) !=
                  FALSE &&
              CreateDirectoryW(extended_path(logs_).c_str(), nullptr) !=
                  FALSE &&
              CreateDirectoryW(extended_path(output_).c_str(), nullptr) !=
                  FALSE,
          "Synthetic portable tree must be created");
    constexpr std::array<std::byte, 4U> marker{
        std::byte{0x4D}, std::byte{0x5A}, std::byte{0x00}, std::byte{0x00}};
    write_bytes(executable_, marker);
  }

  ~TemporaryTree() {
    for (auto iterator = reparse_paths_.rbegin();
         iterator != reparse_paths_.rend();
         ++iterator) {
      static_cast<void>(RemoveDirectoryW(extended_path(*iterator).c_str()));
    }
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TemporaryTree(const TemporaryTree&) = delete;
  TemporaryTree& operator=(const TemporaryTree&) = delete;

  [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
  [[nodiscard]] const std::wstring& application() const noexcept {
    return application_;
  }
  [[nodiscard]] const std::wstring& data() const noexcept { return data_; }
  [[nodiscard]] const std::wstring& logs() const noexcept { return logs_; }
  [[nodiscard]] const std::wstring& output() const noexcept {
    return output_;
  }
  [[nodiscard]] const std::wstring& executable() const noexcept {
    return executable_;
  }
  [[nodiscard]] const std::wstring& final_path() const noexcept {
    return final_;
  }
  [[nodiscard]] std::wstring partial_path() const {
    return final_ + L".partial";
  }
  [[nodiscard]] std::wstring log_path(const std::wstring_view name) const {
    return logs_ + L"\\" + std::wstring(name);
  }
  void track_reparse(std::wstring path) {
    reparse_paths_.push_back(std::move(path));
  }

 private:
  std::wstring root_;
  std::wstring application_;
  std::wstring data_;
  std::wstring logs_;
  std::wstring output_;
  std::wstring executable_;
  std::wstring final_;
  std::vector<std::wstring> reparse_paths_;
};

constexpr std::wstring_view kNormalLog{
    L"TsumugiDrive-normal-20260812-120000-001-1234.log"};
constexpr std::wstring_view kOlderLog{
    L"TsumugiDrive-failed-20260811-120000-001-1235.log"};

void write_log(
    const TemporaryTree& tree,
    const std::wstring_view name,
    const std::string_view body) {
  write_bytes(tree.log_path(name), utf8_bom_log(body));
}

void newest_first_selection_is_bounded_without_large_allocations() {
  constexpr std::uint64_t kMiB = 1024U * 1024U;
  const std::vector<ytec::windowsapp::SupportZipSelectionCandidate>
      candidates{
          {std::wstring(kNormalLog), 40U * kMiB, 30U},
          {L"TsumugiDrive-normal-20260812-120001-001-1234.log",
           40U * kMiB,
           20U},
          {std::wstring(kOlderLog), 4U * kMiB, 10U},
      };
  const auto selected =
      ytec::windowsapp::select_support_zip_candidates(candidates);
  check(selected.has_value(), "A bounded newest-first subset should select");
  check(selected.value().selected_indices == std::vector<std::size_t>{0U, 2U},
        "Newer fitting entries should be selected deterministically");
  check(selected.value().selected_bytes == 44U * kMiB &&
            selected.value().excluded_count == 1U,
        "The plan must expose selected bytes and exclusion count");

  const std::vector<ytec::windowsapp::SupportZipSelectionCandidate>
      retention_boundary{
          {std::wstring(kNormalLog), 200U * kMiB, 20U},
          {std::wstring(kOlderLog), 3U, 10U},
      };
  const auto complete_retained_log =
      ytec::windowsapp::select_support_zip_candidates(retention_boundary);
  check(complete_retained_log.has_value() &&
            complete_retained_log.value().selected_indices ==
                std::vector<std::size_t>{0U} &&
            complete_retained_log.value().selected_bytes == 200U * kMiB &&
            complete_retained_log.value().excluded_count == 1U,
        "A newest valid 200 MiB retained log must remain supportable");

  const std::vector<ytec::windowsapp::SupportZipSelectionCandidate>
      oversize_then_valid{
          {std::wstring(kNormalLog), 200U * kMiB + 1U, 20U},
          {std::wstring(kOlderLog), 3U, 10U},
      };
  const auto skipped =
      ytec::windowsapp::select_support_zip_candidates(oversize_then_valid);
  check(skipped.has_value() &&
            skipped.value().selected_indices == std::vector<std::size_t>{1U} &&
            skipped.value().excluded_count == 1U,
        "An oversized candidate must be excluded without blocking a valid log");
}

void creation_masks_each_line_and_publishes_verified_stored_zip() {
  TemporaryTree tree;
  write_log(
      tree,
      kNormalLog,
      "safe operation stage=verify\r\npassword=secret-value\n"
      "open C:\\Users\\Alice\\Customer.pdf failed\r\n");
  const std::wstring unknown = tree.log_path(L"customer-notes.log");
  const auto unknown_bytes = utf8_bom_log("must remain untouched\n");
  write_bytes(unknown, unknown_bytes);

  const auto plan = ytec::windowsapp::plan_windows_support_zip(
      tree.executable(), tree.final_path());
  check(plan.has_value() && plan.value().entries().size() == 1U &&
            plan.value().entries().front().archive_entry_name == kNormalLog &&
            plan.value().candidate_log_count() == 1U &&
            plan.value().excluded_log_count() == 0U,
        "The review plan must expose only strict basename-only product logs");
  check(plan.value().final_path() == tree.final_path() &&
            plan.value().partial_path() == tree.partial_path(),
        "The plan must expose the local final and adjacent partial paths");

  const auto created = ytec::windowsapp::create_windows_support_zip(
      plan.value());
  if (!created.has_value()) {
    std::cerr << "support ZIP create native="
              << created.error().native_code << " code="
              << static_cast<unsigned int>(created.error().code)
              << " operation=" << utf8(created.error().operation)
              << " message=" << utf8(created.error().message) << '\n';
  }
  check(created.has_value() && created.value().local_only &&
            created.value().entries.size() == 1U &&
            created.value().excluded_log_count == 0U &&
            created.value().final_path == tree.final_path(),
        "Creation must report a local-only archive matching the review plan");
  check(file_exists(tree.final_path()) && !file_exists(tree.partial_path()) &&
            read_bytes(unknown) == unknown_bytes,
        "Only the new final archive may appear; unknown logs stay untouched");

  const auto archive = read_bytes(tree.final_path());
  check(archive.size() == created.value().archive_size_bytes &&
            contains_u32(archive, 0x04034B50U) &&
            contains_u32(archive, 0x02014B50U) &&
            contains_u32(archive, 0x06054B50U),
        "Stored ZIP local, central, and EOCD records must all exist");
  check(archive.size() >= 10U &&
            archive[8U] == std::byte{0x00} &&
            archive[9U] == std::byte{0x00},
        "ZIP compression method must be stored (method zero)");
  check(contains_ascii(archive, "safe operation stage=verify") &&
            contains_ascii(archive, "[PRIVATE]") &&
            contains_ascii(archive, "[PATH]") &&
            !contains_ascii(archive, "secret-value") &&
            !contains_ascii(archive, "Alice") &&
            !contains_ascii(archive, "Customer.pdf"),
        "Every UTF-8 line must receive the product privacy sanitizer");
}

void changed_or_replaced_logs_fail_before_output() {
  {
    TemporaryTree tree;
    write_log(tree, kNormalLog, "stage=initial\n");
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(plan.has_value(), "Tamper fixture must plan");
    write_log(tree, kNormalLog, "stage=tampered-and-longer\n");
    const auto created = ytec::windowsapp::create_windows_support_zip(
        plan.value());
    check(!created.has_value() && !file_exists(tree.final_path()) &&
              !file_exists(tree.partial_path()),
          "In-place tamper after review must fail without an archive");
  }

  {
    TemporaryTree tree;
    write_log(tree, kNormalLog, "stage=original\n");
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(plan.has_value(), "Replacement fixture must plan");
    const std::wstring backup = tree.root() + L"\\original.log";
    check(MoveFileW(
              extended_path(tree.log_path(kNormalLog)).c_str(),
              extended_path(backup).c_str()) != FALSE,
          "The reviewed source must move for replacement simulation");
    write_log(tree, kNormalLog, "stage=replacement\n");
    const auto created = ytec::windowsapp::create_windows_support_zip(
        plan.value());
    check(!created.has_value() && file_exists(backup) &&
              file_exists(tree.log_path(kNormalLog)) &&
              !file_exists(tree.final_path()) &&
              !file_exists(tree.partial_path()),
          "File-ID replacement must preserve both files and fail closed");
  }
}

void foreign_final_and_partial_are_never_overwritten_or_deleted() {
  constexpr std::array<std::byte, 4U> foreign{
      std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  {
    TemporaryTree tree;
    write_log(tree, kNormalLog, "stage=publish-collision\n");
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(plan.has_value(), "Final collision fixture must plan");
    write_bytes(tree.final_path(), foreign);
    const auto created = ytec::windowsapp::create_windows_support_zip(
        plan.value());
    check(!created.has_value() &&
              same_bytes(read_bytes(tree.final_path()), foreign) &&
              !file_exists(tree.partial_path()),
          "A late final collision must preserve final and clean owned partial");
  }

  {
    TemporaryTree tree;
    write_log(tree, kNormalLog, "stage=partial-collision\n");
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(plan.has_value(), "Partial collision fixture must plan");
    write_bytes(tree.partial_path(), foreign);
    const auto created = ytec::windowsapp::create_windows_support_zip(
        plan.value());
    check(!created.has_value() &&
              same_bytes(read_bytes(tree.partial_path()), foreign) &&
              !file_exists(tree.final_path()),
          "CREATE_NEW must preserve a foreign partial exactly");
  }
}

void hardlink_and_strict_utf8_fail_closed() {
  {
    TemporaryTree tree;
    write_log(tree, kNormalLog, "stage=linked\n");
    const std::wstring link = tree.root() + L"\\synthetic-log-link.bin";
    check(CreateHardLinkW(
              extended_path(link).c_str(),
              extended_path(tree.log_path(kNormalLog)).c_str(),
              nullptr) != FALSE,
          "Synthetic hardlink must be created");
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(!plan.has_value() && file_exists(tree.log_path(kNormalLog)) &&
              file_exists(link) && !file_exists(tree.final_path()),
          "A multiply-linked product log must be preserved and rejected");
  }

  {
    TemporaryTree tree;
    const std::array<std::byte, 5U> invalid{
        std::byte{0xEF},
        std::byte{0xBB},
        std::byte{0xBF},
        std::byte{0xC3},
        std::byte{0x28}};
    write_bytes(tree.log_path(kNormalLog), invalid);
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(!plan.has_value() && file_exists(tree.log_path(kNormalLog)) &&
              !file_exists(tree.final_path()),
          "Malformed UTF-8 after a BOM must remain untouched and fail closed");
  }

  {
    TemporaryTree tree;
    const std::array<std::byte, 4U> no_bom{
        std::byte{'l'}, std::byte{'o'}, std::byte{'g'}, std::byte{'\n'}};
    write_bytes(tree.log_path(kNormalLog), no_bom);
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(!plan.has_value() && file_exists(tree.log_path(kNormalLog)),
          "A product-like log without the required BOM must be rejected");
  }
}

#pragma pack(push, 1)
struct MountPointReparseData final {
  DWORD reparse_tag{};
  WORD reparse_data_length{};
  WORD reserved{};
  WORD substitute_name_offset{};
  WORD substitute_name_length{};
  WORD print_name_offset{};
  WORD print_name_length{};
  wchar_t path_buffer[1]{};
};
#pragma pack(pop)

void create_directory_junction(
    TemporaryTree& tree,
    const std::wstring& junction,
    const std::wstring& target) {
  check(CreateDirectoryW(extended_path(junction).c_str(), nullptr) != FALSE,
        "Synthetic junction directory must be created");
  tree.track_reparse(junction);
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      extended_path(junction).c_str(),
      GENERIC_WRITE,
      0U,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr));
  check(handle.valid(), "Synthetic junction handle must open");
  const std::wstring substitute = L"\\??\\" + target;
  const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
  const std::size_t print_bytes = target.size() * sizeof(wchar_t);
  const std::size_t path_bytes =
      substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
  check(path_bytes <= (std::numeric_limits<WORD>::max)() - 8U,
        "Synthetic junction data must fit the reparse format");
  const std::size_t allocation =
      offsetof(MountPointReparseData, path_buffer) + path_bytes;
  std::vector<std::byte> storage(allocation, std::byte{0});
  auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
  data->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
  data->substitute_name_length = static_cast<WORD>(substitute_bytes);
  data->print_name_offset =
      static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
  data->print_name_length = static_cast<WORD>(print_bytes);
  data->reparse_data_length = static_cast<WORD>(8U + path_bytes);
  std::memcpy(data->path_buffer, substitute.data(), substitute_bytes);
  std::memcpy(
      reinterpret_cast<std::byte*>(data->path_buffer) +
          data->print_name_offset,
      target.data(),
      print_bytes);
  DWORD returned{};
  check(DeviceIoControl(
            handle.get(),
            FSCTL_SET_REPARSE_POINT,
            data,
            static_cast<DWORD>(allocation),
            nullptr,
            0U,
            &returned,
            nullptr) != FALSE,
        "Synthetic junction must be created without following its target");
}

void reparse_and_directory_replacement_fail_closed() {
  {
    TemporaryTree tree;
    check(RemoveDirectoryW(extended_path(tree.logs()).c_str()) != FALSE,
          "Empty logs directory must be removed for the junction fixture");
    const std::wstring real_logs = tree.root() + L"\\real-logs";
    check(CreateDirectoryW(extended_path(real_logs).c_str(), nullptr) != FALSE,
          "Synthetic junction target must be created");
    create_directory_junction(tree, tree.logs(), real_logs);
    write_bytes(
        real_logs + L"\\" + std::wstring(kNormalLog),
        utf8_bom_log("stage=reparse\n"));
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(!plan.has_value() && !file_exists(tree.final_path()),
          "A reparse point anywhere in the logs chain must fail closed");
  }

  {
    TemporaryTree tree;
    write_log(tree, kNormalLog, "stage=directory-replacement\n");
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(plan.has_value(), "Directory replacement fixture must plan");
    check(RemoveDirectoryW(extended_path(tree.output()).c_str()) != FALSE &&
              CreateDirectoryW(
                  extended_path(tree.output()).c_str(), nullptr) != FALSE,
          "Output directory must be replaced by a new object");
    const auto created = ytec::windowsapp::create_windows_support_zip(
        plan.value());
    check(!created.has_value() && !file_exists(tree.final_path()) &&
              !file_exists(tree.partial_path()),
          "Output directory File-ID replacement must fail before writing");
  }
}

void create_sparse_oversized_log(const std::wstring& path) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      0U,
      nullptr,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  check(file.valid(), "Sparse oversized fixture must open");
  DWORD returned{};
  check(DeviceIoControl(
            file.get(),
            FSCTL_SET_SPARSE,
            nullptr,
            0U,
            nullptr,
            0U,
            &returned,
            nullptr) != FALSE,
        "Oversized fixture must use sparse storage");
  constexpr std::array<std::byte, 3U> bom{
      std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};
  DWORD written{};
  check(WriteFile(
            file.get(),
            bom.data(),
            static_cast<DWORD>(bom.size()),
            &written,
            nullptr) != FALSE &&
            written == static_cast<DWORD>(bom.size()),
        "Sparse oversized fixture must start with the product BOM");
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(
      ytec::windowsapp::kSupportZipMaximumSourceFileBytes + 1U);
  check(SetFilePointerEx(file.get(), end, nullptr, FILE_BEGIN) != FALSE &&
            SetEndOfFile(file.get()) != FALSE &&
            FlushFileBuffers(file.get()) != FALSE,
        "Sparse oversized fixture must set only a logical length");
}

void oversize_empty_and_path_boundaries_fail_closed() {
  {
    TemporaryTree tree;
    create_sparse_oversized_log(tree.log_path(kNormalLog));
    const auto plan = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(!plan.has_value() && file_exists(tree.log_path(kNormalLog)) &&
              !file_exists(tree.final_path()),
          "An oversized sparse product log must be preserved and excluded");
  }

  {
    TemporaryTree tree;
    write_bytes(
        tree.log_path(L"customer.log"), utf8_bom_log("unknown\n"));
    const auto empty = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.final_path());
    check(!empty.has_value(),
          "Unknown logs alone must never become support archive entries");
    const auto inside_data = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.data() + L"\\support.zip");
    check(!inside_data.has_value(),
          "A final archive inside portable data must be rejected");
    const auto traversal = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.output() + L"\\..\\escape.zip");
    check(!traversal.has_value(),
          "A path containing traversal must be rejected before I/O");
    const auto appdata = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.root() + L"\\AppData\\support.zip");
    check(!appdata.has_value(), "AppData output must be rejected");
    const auto unc = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), L"\\\\server\\share\\support.zip");
    check(!unc.has_value(), "UNC output must be rejected");
    const auto wrong_extension = ytec::windowsapp::plan_windows_support_zip(
        tree.executable(), tree.output() + L"\\support.txt");
    check(!wrong_extension.has_value(),
          "Only a local absolute .zip final path may be planned");
  }
}

}  // namespace

int main() {
  try {
    newest_first_selection_is_bounded_without_large_allocations();
    creation_masks_each_line_and_publishes_verified_stored_zip();
    changed_or_replaced_logs_fail_before_output();
    foreign_final_and_partial_are_never_overwritten_or_deleted();
    hardlink_and_strict_utf8_fail_closed();
    reparse_and_directory_replacement_fail_closed();
    oversize_empty_and_path_boundaries_fail_closed();
    std::cout << "support_zip_tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "support_zip_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
}

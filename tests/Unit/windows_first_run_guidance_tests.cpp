#include "first_run_guidance_internal.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/windowsapp/first_run_guidance.h"
#include "ytec/windowsapp/layout.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
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

#ifndef YTEC_WINDOWS_APP_MAIN_SOURCE_PATH
#error YTEC_WINDOWS_APP_MAIN_SOURCE_PATH must name the product main source
#endif

namespace {

struct TestFailure final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

bool path_exists(const std::wstring& path) {
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
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  check(file.valid(), "Synthetic settings file must be created");
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    DWORD written{};
    const DWORD requested = static_cast<DWORD>(bytes.size() - consumed);
    check(WriteFile(
              file.get(),
              bytes.data() + consumed,
              requested,
              &written,
              nullptr) != FALSE &&
              written != 0U,
          "Synthetic settings bytes must be written");
    consumed += static_cast<std::size_t>(written);
  }
  check(FlushFileBuffers(file.get()) != FALSE,
        "Synthetic settings bytes must be flushed");
}

std::vector<std::byte> read_bytes(const std::wstring& path) {
  ytec::clonecore::UniqueHandle file(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  check(file.valid(), "Synthetic settings file must open for reading");
  LARGE_INTEGER size{};
  check(GetFileSizeEx(file.get(), &size) != FALSE &&
            size.QuadPart >= 0 && size.QuadPart <= 1024,
        "Synthetic settings size must remain bounded");
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(size.QuadPart));
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    DWORD read{};
    check(ReadFile(
              file.get(),
              bytes.data() + consumed,
              static_cast<DWORD>(bytes.size() - consumed),
              &read,
              nullptr) != FALSE &&
              read != 0U,
          "Synthetic settings bytes must be read");
    consumed += static_cast<std::size_t>(read);
  }
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
        "Test module path must have a parent directory");
  return path.substr(0U, separator);
}

class TemporaryTree final {
 public:
  explicit TemporaryTree(const bool create_data = true) {
    static unsigned int sequence{};
    const std::wstring parent = module_directory();
    for (unsigned int attempt = 0U; attempt < 100U; ++attempt) {
      ++sequence;
      const std::wstring candidate =
          parent + L"\\first-run-guidance-test-" +
          std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(sequence);
      if (CreateDirectoryW(extended_path(candidate).c_str(), nullptr) !=
          FALSE) {
        root_ = candidate;
        break;
      }
      check(GetLastError() == ERROR_ALREADY_EXISTS,
            "Synthetic root may only collide with a prior fixture");
    }
    check(!root_.empty(), "Synthetic portable root must be created");
    application_ = root_ + L"\\app";
    data_ = application_ + L"\\data";
    final_ = data_ + L"\\" +
        std::wstring(
            ytec::windowsapp::kFirstRunGuidanceSettingsFileName);
    check(CreateDirectoryW(
              extended_path(application_).c_str(), nullptr) != FALSE,
          "Synthetic application directory must be created");
    if (create_data) {
      check(CreateDirectoryW(
                extended_path(data_).c_str(), nullptr) != FALSE,
            "Synthetic data directory must be created");
    }
  }

  ~TemporaryTree() {
    static_cast<void>(SetFileAttributesW(
        extended_path(final_).c_str(), FILE_ATTRIBUTE_NORMAL));
    for (auto iterator = reparse_paths_.rbegin();
         iterator != reparse_paths_.rend();
         ++iterator) {
      static_cast<void>(RemoveDirectoryW(
          extended_path(*iterator).c_str()));
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
  [[nodiscard]] const std::wstring& final_path() const noexcept {
    return final_;
  }
  [[nodiscard]] std::wstring stage_path() const {
    return final_ + L".partial";
  }
  [[nodiscard]] std::wstring backup_path() const {
    return final_ + L".backup";
  }
  void track_reparse(std::wstring path) {
    reparse_paths_.push_back(std::move(path));
  }

 private:
  std::wstring root_;
  std::wstring application_;
  std::wstring data_;
  std::wstring final_;
  std::vector<std::wstring> reparse_paths_;
};

void write_document(TemporaryTree& tree, const bool acknowledged) {
  const auto document =
      ytec::windowsapp::serialize_first_run_guidance_document(
          acknowledged);
  write_bytes(tree.final_path(), document);
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
  const std::size_t path_bytes = substitute_bytes + sizeof(wchar_t) +
                                 print_bytes + sizeof(wchar_t);
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

std::size_t count_token(
    const std::string_view text,
    const std::string_view token) {
  std::size_t count = 0U;
  for (std::size_t offset = text.find(token);
       offset != std::string_view::npos;
       offset = text.find(token, offset + token.size())) {
    ++count;
  }
  return count;
}

std::string read_product_main_source() {
  std::ifstream source(
      YTEC_WINDOWS_APP_MAIN_SOURCE_PATH,
      std::ios::binary);
  check(source.good(), "Product main source must be readable");
  return std::string(
      std::istreambuf_iterator<char>(source),
      std::istreambuf_iterator<char>());
}

void three_items_schema_and_compact_layout_are_fixed() {
  using namespace ytec::windowsapp;
  check(kFirstRunGuidanceItems.size() == 3U,
        "The first-run guidance must contain exactly three items");
  check(kFirstRunGuidanceItems[0].title == L"対象確認" &&
            kFirstRunGuidanceItems[0].description.find(L"モデル") !=
                std::wstring_view::npos &&
            kFirstRunGuidanceItems[0].description.find(L"容量") !=
                std::wstring_view::npos &&
            kFirstRunGuidanceItems[0].description.find(L"接続方式") !=
                std::wstring_view::npos &&
            kFirstRunGuidanceItems[0].description.find(L"シリアル末尾") !=
                std::wstring_view::npos,
        "Target guidance must identify model, capacity, bus and serial suffix");
  check(kFirstRunGuidanceItems[1].title == L"元ディスク保護" &&
            kFirstRunGuidanceItems[1].description.find(
                L"アプリはコピー元へ直接書き込みません") !=
                std::wstring_view::npos &&
            kFirstRunGuidanceItems[1].description.find(
                L"完全無変更が必要ならレスキューメディア") !=
                std::wstring_view::npos &&
            kFirstRunGuidanceItems[1].description.find(
                L"コピー元は読み取り専用") == std::wstring_view::npos,
        "Windows guidance must not claim that the live source is read-only");
  check(kFirstRunGuidanceItems[2].title == L"検証結果の意味" &&
            kFirstRunGuidanceItems[2].description.find(L"読戻し") !=
                std::wstring_view::npos &&
            kFirstRunGuidanceItems[2].description.find(L"起動成功") !=
                std::wstring_view::npos,
        "Verification guidance must separate readback from boot success");

  const auto pending = serialize_first_run_guidance_document(false);
  const auto acknowledged = serialize_first_run_guidance_document(true);
  check(pending.size() == kFirstRunGuidanceDocumentBytes &&
            classify_first_run_guidance_document(pending) ==
                FirstRunGuidanceDocumentState::acknowledgement_pending &&
            classify_first_run_guidance_document(acknowledged) ==
                FirstRunGuidanceDocumentState::acknowledged,
        "The fixed schema-v1 document must round-trip without text parsing");
  auto malformed = acknowledged;
  malformed[13U] = std::byte{0x01};
  check(classify_first_run_guidance_document(malformed) ==
            FirstRunGuidanceDocumentState::malformed,
        "Non-zero reserved bytes must fail closed");
  auto newer = acknowledged;
  newer[8U] = std::byte{0x02};
  check(classify_first_run_guidance_document(newer) ==
            FirstRunGuidanceDocumentState::newer_schema,
        "A newer schema must be distinguished without migration");
  check(plan_first_run_guidance(
            FirstRunGuidanceDocumentState::missing)
            .acknowledgement_may_be_saved &&
            !plan_first_run_guidance(
                 FirstRunGuidanceDocumentState::acknowledged)
                 .show_guidance &&
            !plan_first_run_guidance(
                 FirstRunGuidanceDocumentState::malformed)
                 .acknowledgement_may_be_saved &&
            !plan_first_run_guidance(
                 FirstRunGuidanceDocumentState::newer_schema)
                 .acknowledgement_may_be_saved &&
            plan_first_run_guidance(
                FirstRunGuidanceDocumentState::storage_unavailable)
                .show_guidance,
        "Unknown or unavailable settings must show but never auto-migrate");

  for (const int client_width : {960, 1024, 1280}) {
    const auto bottom = calculate_bottom_action_layout(client_width);
    const auto buttons =
        calculate_first_run_guidance_diagnostic_button_layout(
            bottom.secondary_action.left,
            bottom.secondary_action.right);
    check(buttons.valid() && buttons.update_width >= 90 &&
              buttons.guidance_width >= 90 &&
              buttons.guidance_left + buttons.guidance_width ==
                  bottom.secondary_action.right,
          "Both diagnostics buttons must remain reachable at compact widths");
  }
}

void first_launch_acknowledgement_and_saved_launch_are_real() {
  TemporaryTree tree;
  auto first = ytec::windowsapp::detail::
      inspect_first_run_guidance_in_existing_data_directory(tree.data());
  check(first.state ==
            ytec::windowsapp::FirstRunGuidanceDocumentState::missing &&
            first.decision.show_guidance &&
            first.decision.acknowledgement_may_be_saved,
        "A missing setting must show the first-run guidance");

  auto saved = ytec::windowsapp::detail::
      save_first_run_guidance_in_existing_data_directory(tree.data());
  check(saved.has_value() &&
            saved.value().disposition ==
                ytec::windowsapp::FirstRunGuidanceSaveDisposition::created &&
            !saved.value().recovery_backup_retained &&
            path_exists(tree.final_path()) &&
            !path_exists(tree.stage_path()) &&
            !path_exists(tree.backup_path()),
        "The first acknowledgement must publish exactly the fixed final file");

  const auto later = ytec::windowsapp::detail::
      inspect_first_run_guidance_in_existing_data_directory(tree.data());
  check(later.state ==
            ytec::windowsapp::FirstRunGuidanceDocumentState::acknowledged &&
            !later.decision.show_guidance,
        "An exact schema-v1 acknowledgement must suppress only first display");
  const auto again = ytec::windowsapp::detail::
      save_first_run_guidance_in_existing_data_directory(tree.data());
  check(again.has_value() &&
            again.value().disposition ==
                ytec::windowsapp::FirstRunGuidanceSaveDisposition::
                    already_acknowledged,
        "An acknowledged setting must not be overwritten");
}

void known_pending_document_uses_recoverable_replace() {
  TemporaryTree tree;
  write_document(tree, false);
  const auto original = read_bytes(tree.final_path());
  auto saved = ytec::windowsapp::detail::
      save_first_run_guidance_in_existing_data_directory(tree.data());
  if (!saved) {
    std::cerr << "pending replace native="
              << saved.error().native_code << " code="
              << static_cast<unsigned int>(saved.error().code) << '\n';
  }
  check(saved.has_value() &&
            saved.value().disposition ==
                ytec::windowsapp::FirstRunGuidanceSaveDisposition::replaced &&
            !saved.value().recovery_backup_retained &&
            read_bytes(tree.final_path()) != original &&
            !path_exists(tree.stage_path()) &&
            !path_exists(tree.backup_path()),
        "Only an exact pending schema-v1 file may be recoverably replaced");
  check(ytec::windowsapp::classify_first_run_guidance_document(
            read_bytes(tree.final_path())) ==
            ytec::windowsapp::FirstRunGuidanceDocumentState::acknowledged,
        "The replaced file must be the exact acknowledged document");
}

void malformed_newer_and_foreign_stage_are_preserved() {
  {
    TemporaryTree tree;
    const std::array<std::byte, 5U> malformed{
        std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
        std::byte{0x44}, std::byte{0x45}};
    write_bytes(tree.final_path(), malformed);
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::malformed &&
              inspected.decision.show_guidance &&
              !inspected.decision.acknowledgement_may_be_saved &&
              !saved.has_value() && read_bytes(tree.final_path()) ==
                  std::vector<std::byte>(malformed.begin(), malformed.end()),
          "Malformed settings must be shown, preserved and never migrated");
  }
  {
    TemporaryTree tree;
    auto newer =
        ytec::windowsapp::serialize_first_run_guidance_document(true);
    newer[8U] = std::byte{0x02};
    write_bytes(tree.final_path(), newer);
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::newer_schema &&
              !saved.has_value() && read_bytes(tree.final_path()) ==
                  std::vector<std::byte>(newer.begin(), newer.end()),
          "Newer settings must be preserved without down-migration");
  }
  {
    TemporaryTree tree;
    const std::array<std::byte, 4U> foreign{
        std::byte{0x46}, std::byte{0x4F},
        std::byte{0x52}, std::byte{0x45}};
    write_bytes(tree.stage_path(), foreign);
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(!saved.has_value() && !path_exists(tree.final_path()) &&
              read_bytes(tree.stage_path()) ==
                  std::vector<std::byte>(foreign.begin(), foreign.end()),
          "A foreign fixed partial must be preserved by CREATE_NEW");
  }
  {
    TemporaryTree tree;
    write_document(tree, false);
    const auto original = read_bytes(tree.final_path());
    const std::array<std::byte, 4U> foreign{
        std::byte{0x42}, std::byte{0x41},
        std::byte{0x43}, std::byte{0x4B}};
    write_bytes(tree.backup_path(), foreign);
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(!saved.has_value() && read_bytes(tree.final_path()) == original &&
              read_bytes(tree.backup_path()) ==
                  std::vector<std::byte>(foreign.begin(), foreign.end()) &&
              !path_exists(tree.stage_path()),
          "A foreign fixed backup must be preserved by non-overwrite rename");
  }
}

void hardlink_read_only_and_locked_files_fail_closed() {
  {
    TemporaryTree tree;
    write_document(tree, false);
    const std::wstring link = tree.root() + L"\\linked-settings.bin";
    check(CreateHardLinkW(
              extended_path(link).c_str(),
              extended_path(tree.final_path()).c_str(),
              nullptr) != FALSE,
          "Synthetic hardlink must be created");
    const auto original = read_bytes(tree.final_path());
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::
                  storage_unavailable &&
              !saved.has_value() && read_bytes(tree.final_path()) == original &&
              read_bytes(link) == original,
          "A hardlinked setting must be preserved and rejected");
  }
  {
    TemporaryTree tree;
    write_document(tree, false);
    const auto original = read_bytes(tree.final_path());
    check(SetFileAttributesW(
              extended_path(tree.final_path()).c_str(),
              FILE_ATTRIBUTE_READONLY) != FALSE,
          "Synthetic setting must become read-only");
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::
                  storage_unavailable &&
              !saved.has_value(),
          "A read-only setting must remain unavailable and unmodified");
    check(SetFileAttributesW(
              extended_path(tree.final_path()).c_str(),
              FILE_ATTRIBUTE_NORMAL) != FALSE &&
              read_bytes(tree.final_path()) == original,
          "The read-only fixture must retain its original bytes");
  }
  {
    TemporaryTree tree;
    write_document(tree, false);
    const auto original = read_bytes(tree.final_path());
    ytec::clonecore::UniqueHandle lock(CreateFileW(
        extended_path(tree.final_path()).c_str(),
        GENERIC_READ,
        0U,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    check(lock.valid(), "Synthetic exclusive setting lock must open");
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::
                  storage_unavailable &&
              !saved.has_value(),
          "An exclusively locked setting must fail closed");
    lock.reset();
    check(read_bytes(tree.final_path()) == original,
          "A locked setting must retain its original bytes");
  }
}

struct ReplaceCollisionContext final {
  std::wstring path;
  ytec::clonecore::UniqueHandle handle;
};

void create_foreign_final_before_publish(void* const raw_context) noexcept {
  auto* context = static_cast<ReplaceCollisionContext*>(raw_context);
  context->handle.reset(CreateFileW(
      extended_path(context->path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!context->handle) {
    return;
  }
  constexpr std::array<std::byte, 4U> foreign{
      std::byte{0x52}, std::byte{0x41},
      std::byte{0x43}, std::byte{0x45}};
  DWORD written{};
  static_cast<void>(WriteFile(
      context->handle.get(),
      foreign.data(),
      static_cast<DWORD>(foreign.size()),
      &written,
      nullptr));
  static_cast<void>(FlushFileBuffers(context->handle.get()));
}

void replace_failure_preserves_original_and_cleans_owned_files() {
  TemporaryTree tree;
  write_document(tree, false);
  const auto original = read_bytes(tree.final_path());
  ReplaceCollisionContext context{.path = tree.final_path()};
  const auto saved = ytec::windowsapp::detail::
      save_first_run_guidance_in_existing_data_directory(
          tree.data(), create_foreign_final_before_publish, &context);
  check(context.handle.valid(),
        "The replace hook must create a real competing final file");
  check(!saved.has_value(), "A real non-overwriting publish must fail");
  context.handle.reset();
  constexpr std::array<std::byte, 4U> foreign{
      std::byte{0x52}, std::byte{0x41},
      std::byte{0x43}, std::byte{0x45}};
  check(read_bytes(tree.final_path()) ==
                std::vector<std::byte>(foreign.begin(), foreign.end()) &&
            read_bytes(tree.backup_path()) == original &&
            !path_exists(tree.stage_path()),
        "Publish collision must preserve the foreign final and original backup");
}

void missing_reparse_and_appdata_storage_never_fall_back() {
  {
    TemporaryTree tree(false);
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::
                  storage_unavailable &&
              inspected.decision.show_guidance && !saved.has_value() &&
              !path_exists(tree.data()),
          "A missing data directory must not be created or bypassed");
  }
  {
    TemporaryTree tree;
    check(RemoveDirectoryW(extended_path(tree.data()).c_str()) != FALSE,
          "Empty synthetic data must be removed for the reparse fixture");
    const std::wstring target = tree.root() + L"\\real-data";
    check(CreateDirectoryW(extended_path(target).c_str(), nullptr) != FALSE,
          "Synthetic reparse target must be created");
    create_directory_junction(tree, tree.data(), target);
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(tree.data());
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(tree.data());
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::
                  storage_unavailable &&
              !saved.has_value() &&
              !path_exists(
                  target + L"\\" + std::wstring(
                      ytec::windowsapp::kFirstRunGuidanceSettingsFileName)),
          "A data reparse point must fail before any target write");
  }
  {
    TemporaryTree tree;
    const std::wstring appdata =
        tree.root() + L"\\AppData\\portable\\data";
    const auto inspected = ytec::windowsapp::detail::
        inspect_first_run_guidance_in_existing_data_directory(appdata);
    const auto saved = ytec::windowsapp::detail::
        save_first_run_guidance_in_existing_data_directory(appdata);
    check(inspected.state ==
              ytec::windowsapp::FirstRunGuidanceDocumentState::
                  storage_unavailable &&
              !saved.has_value() &&
              !path_exists(tree.root() + L"\\AppData"),
          "AppData paths must perform zero creation and zero fallback writes");
  }
}

void product_source_connects_current_exe_factory_and_both_ui_paths() {
  const std::string source = read_product_main_source();
  const std::size_t update = source.find("UpdateWindow(window);");
  const std::size_t startup =
      source.find("show_first_run_guidance_if_needed(state);");
  check(update != std::string::npos && startup != std::string::npos &&
            update < startup &&
            count_token(source,
                        "show_first_run_guidance_if_needed(state);") == 1U,
        "Product startup must show guidance only after the main window exists");
  check(count_token(source, "inspect_windows_first_run_guidance()") == 1U &&
            count_token(
                source,
                "save_windows_first_run_guidance_acknowledgement()") == 1U &&
            count_token(
                source,
                "inspect_first_run_guidance_in_existing_data_directory") ==
                0U &&
            count_token(
                source,
                "save_first_run_guidance_in_existing_data_directory") == 0U,
        "Product main must use only the current-EXE production wrappers");
  check(source.find("TDCBF_OK_BUTTON | TDCBF_CLOSE_BUTTON") !=
                std::string::npos &&
            source.find("TDF_ALLOW_DIALOG_CANCELLATION") !=
                std::string::npos &&
            source.find("TaskDialogIndirect") != std::string::npos &&
            source.find("restore_error_dialog_focus(previous_focus)") !=
                std::string::npos,
        "Native guidance must support OK, Close, Esc and focus restoration");
  check(source.find("kFirstRunGuidanceActionId") != std::string::npos &&
            count_token(source, "first_run_guidance_action") >= 8U &&
            source.find("show_first_run_guidance_dialog(*state, false)") !=
                std::string::npos &&
            source.find("calculate_diagnostics_action_layout") !=
                std::string::npos,
        "Diagnostics must expose the same guide in its own action row");
  check(source.find("SHGetKnownFolderPath") == std::string::npos &&
            source.find("FOLDERID_LocalAppData") == std::string::npos &&
            source.find("RegSetValue") == std::string::npos,
        "First-run wiring must not add AppData or registry persistence");
}

}  // namespace

int main() {
  try {
    three_items_schema_and_compact_layout_are_fixed();
    first_launch_acknowledgement_and_saved_launch_are_real();
    known_pending_document_uses_recoverable_replace();
    malformed_newer_and_foreign_stage_are_preserved();
    hardlink_read_only_and_locked_files_fail_closed();
    replace_failure_preserves_original_and_cleans_owned_files();
    missing_reparse_and_appdata_storage_never_fall_back();
    product_source_connects_current_exe_factory_and_both_ui_paths();
  } catch (const std::exception& error) {
    std::cerr << "windows first-run guidance test failed: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "windows first-run guidance tests passed\n";
  return 0;
}

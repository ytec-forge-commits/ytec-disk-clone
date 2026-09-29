#include "ytec/bootrepair/efi_boot_ownership.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/offline_windows.h"
#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::size_t kMaximumDirectoryEntries = 4'096U;
constexpr std::size_t kMaximumDirectoryDepth = 8U;

clonecore::Error ownership_error(
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

bool same_text(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t left_character, const wchar_t right_character) {
            return std::towlower(left_character) ==
                std::towlower(right_character);
          });
}

bool ends_with_efi(const std::wstring_view name) {
  constexpr std::wstring_view extension = L".efi";
  return name.size() >= extension.size() &&
      same_text(name.substr(name.size() - extension.size()), extension);
}

bool safe_entry_name(const std::wstring_view name) {
  return !name.empty() && name != L"." && name != L".." &&
      name.find_first_of(L"\\/") == std::wstring_view::npos;
}

enum class PathObjectKind : std::uint8_t {
  missing,
  directory,
  regular_file,
  reparse,
  other,
};

PathObjectKind kind_from_attributes(const DWORD attributes) {
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return PathObjectKind::reparse;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    return PathObjectKind::directory;
  }
  if ((attributes & FILE_ATTRIBUTE_DEVICE) == 0U) {
    return PathObjectKind::regular_file;
  }
  return PathObjectKind::other;
}

clonecore::Result<PathObjectKind> inspect_path_object(
    const std::wstring& path) {
  clonecore::UniqueHandle object(CreateFileW(
      path.c_str(),
      0U,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!object) {
    const DWORD native_code = GetLastError();
    if (native_code == ERROR_FILE_NOT_FOUND ||
        native_code == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<PathObjectKind>::success(
          PathObjectKind::missing);
    }
    return clonecore::Result<PathObjectKind>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ESP内オブジェクト読取り専用確認",
            native_code));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          object.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Result<PathObjectKind>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ESP内オブジェクト属性確認",
            GetLastError()));
  }
  return clonecore::Result<PathObjectKind>::success(
      kind_from_attributes(attributes.FileAttributes));
}

struct DirectoryEntry final {
  std::wstring name;
  PathObjectKind kind{PathObjectKind::other};
};

class UniqueFindHandle final {
 public:
  explicit UniqueFindHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueFindHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      FindClose(handle_);
    }
  }
  UniqueFindHandle(const UniqueFindHandle&) = delete;
  UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

clonecore::Result<std::vector<DirectoryEntry>> enumerate_directory_read_only(
    const std::wstring& directory) {
  WIN32_FIND_DATAW data{};
  const std::wstring pattern = directory + L"\\*";
  const HANDLE raw = FindFirstFileW(pattern.c_str(), &data);
  if (raw == INVALID_HANDLE_VALUE) {
    return clonecore::Result<std::vector<DirectoryEntry>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ESPディレクトリ読取り専用列挙",
            GetLastError()));
  }
  UniqueFindHandle find(raw);
  std::vector<DirectoryEntry> entries;
  while (true) {
    const std::wstring_view name(data.cFileName);
    if (name != L"." && name != L"..") {
      if (!safe_entry_name(name) ||
          entries.size() >= kMaximumDirectoryEntries) {
        return clonecore::Result<std::vector<DirectoryEntry>>::failure(
            ownership_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"ESPディレクトリ列挙上限",
                L"ESP内の名前または項目数が安全な解析上限を超えています"));
      }
      entries.push_back(DirectoryEntry{
          .name = std::wstring(name),
          .kind = kind_from_attributes(data.dwFileAttributes),
      });
    }
    if (FindNextFileW(raw, &data) == FALSE) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_NO_MORE_FILES) {
        break;
      }
      return clonecore::Result<std::vector<DirectoryEntry>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"ESPディレクトリ読取り専用列挙継続",
              native_code));
    }
  }
  return clonecore::Result<std::vector<DirectoryEntry>>::success(
      std::move(entries));
}

clonecore::Status increment_counter(std::uint32_t& counter) {
  if (counter == (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Status::failure(ownership_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"ESP所有権項目数",
        L"ESP所有権の項目数が表現上限を超えました"));
  }
  ++counter;
  return clonecore::success_status();
}

clonecore::Status observe_efi_loader_signature(
    const std::wstring& path,
    IExecutableTrustVerifier& trust_verifier,
    EfiBootOwnershipObservation& observation,
    bool* const microsoft_signed) {
  const clonecore::Status trust =
      trust_verifier.verify_microsoft_signed(path);
  if (trust) {
    const clonecore::Status counted = increment_counter(
        observation.microsoft_signed_efi_loader_count);
    if (!counted) {
      return counted;
    }
    if (microsoft_signed != nullptr) {
      *microsoft_signed = true;
    }
    return clonecore::success_status();
  }
  const clonecore::Status counted = increment_counter(
      observation.non_microsoft_or_untrusted_efi_loader_count);
  if (!counted) {
    return counted;
  }
  if (microsoft_signed != nullptr) {
    *microsoft_signed = false;
  }
  return clonecore::success_status();
}

struct PendingDirectory final {
  std::wstring path;
  std::size_t depth{};
};

clonecore::Status scan_microsoft_namespace_read_only(
    const std::wstring& root,
    IExecutableTrustVerifier& trust_verifier,
    EfiBootOwnershipObservation& observation) {
  std::vector<PendingDirectory> pending{{root, 0U}};
  std::size_t visited = 0U;
  while (!pending.empty()) {
    PendingDirectory current = std::move(pending.back());
    pending.pop_back();
    auto entries = enumerate_directory_read_only(current.path);
    if (!entries) {
      return clonecore::Status::failure(entries.error());
    }
    for (const auto& entry : entries.value()) {
      if (++visited > kMaximumDirectoryEntries) {
        return clonecore::Status::failure(ownership_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_BUFFER_OVERFLOW,
            L"Microsoft EFI領域の列挙上限",
            L"Microsoft EFI領域の項目数が安全な解析上限を超えています"));
      }
      const std::wstring path = current.path + L"\\" + entry.name;
      if (entry.kind == PathObjectKind::reparse ||
          entry.kind == PathObjectKind::other) {
        observation.ambiguous_object_detected = true;
        continue;
      }
      if (entry.kind == PathObjectKind::directory) {
        if (current.depth >= kMaximumDirectoryDepth) {
          observation.ambiguous_object_detected = true;
        } else {
          pending.push_back(PendingDirectory{path, current.depth + 1U});
        }
        continue;
      }
      if (entry.kind == PathObjectKind::regular_file &&
          ends_with_efi(entry.name)) {
        const clonecore::Status signature = observe_efi_loader_signature(
            path, trust_verifier, observation, nullptr);
        if (!signature) {
          return signature;
        }
      }
    }
  }
  return clonecore::success_status();
}

class WindowsEfiBootOwnershipInspector final
    : public IEfiBootOwnershipInspector {
 public:
  explicit WindowsEfiBootOwnershipInspector(
      std::unique_ptr<IExecutableTrustVerifier> trust_verifier) noexcept
      : trust_verifier_(std::move(trust_verifier)) {}

  clonecore::Result<EfiBootOwnershipEvidence>
  inspect_existing_esp_read_only(const std::wstring& volume_root) override {
    const auto normalized = normalize_offline_windows_volume_root(volume_root);
    if (!normalized) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          normalized.error());
    }
    if (trust_verifier_ == nullptr) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          ownership_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_STATE,
              L"ESP EFI署名検証初期化",
              L"Microsoft署名検証サービスを初期化できません"));
    }

    EfiBootOwnershipObservation observation;
    const std::wstring efi_root = normalized.value() + L"EFI";
    auto efi_kind = inspect_path_object(efi_root);
    if (!efi_kind) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          efi_kind.error());
    }
    if (efi_kind.value() == PathObjectKind::missing) {
      return classify_efi_boot_ownership(observation);
    }
    observation.efi_directory_present = true;
    if (efi_kind.value() != PathObjectKind::directory) {
      observation.ambiguous_object_detected = true;
      return classify_efi_boot_ownership(observation);
    }

    auto top_entries = enumerate_directory_read_only(efi_root);
    if (!top_entries) {
      return clonecore::Result<EfiBootOwnershipEvidence>::failure(
          top_entries.error());
    }
    bool microsoft_seen = false;
    bool boot_seen = false;
    for (const auto& entry : top_entries.value()) {
      const std::wstring path = efi_root + L"\\" + entry.name;
      if (same_text(entry.name, L"Microsoft")) {
        if (microsoft_seen || entry.kind != PathObjectKind::directory) {
          observation.ambiguous_object_detected = true;
          continue;
        }
        microsoft_seen = true;
        observation.microsoft_namespace_present = true;
        const clonecore::Status scanned =
            scan_microsoft_namespace_read_only(
                path, *trust_verifier_, observation);
        if (!scanned) {
          return clonecore::Result<EfiBootOwnershipEvidence>::failure(
              scanned.error());
        }
        continue;
      }
      if (same_text(entry.name, L"Boot")) {
        if (boot_seen || entry.kind != PathObjectKind::directory) {
          observation.ambiguous_object_detected = true;
          continue;
        }
        boot_seen = true;
        observation.boot_namespace_present = true;
        auto boot_entries = enumerate_directory_read_only(path);
        if (!boot_entries) {
          return clonecore::Result<EfiBootOwnershipEvidence>::failure(
              boot_entries.error());
        }
        for (const auto& boot_entry : boot_entries.value()) {
          const std::wstring boot_path = path + L"\\" + boot_entry.name;
          if (same_text(boot_entry.name, L"bootx64.efi")) {
            observation.fallback_loader_present = true;
            if (boot_entry.kind != PathObjectKind::regular_file) {
              observation.ambiguous_object_detected = true;
              continue;
            }
            const clonecore::Status signature = observe_efi_loader_signature(
                boot_path,
                *trust_verifier_,
                observation,
                &observation.fallback_loader_microsoft_signed);
            if (!signature) {
              return clonecore::Result<EfiBootOwnershipEvidence>::failure(
                  signature.error());
            }
            continue;
          }
          const clonecore::Status counted = increment_counter(
              observation.boot_namespace_nonstandard_entries);
          if (!counted) {
            return clonecore::Result<EfiBootOwnershipEvidence>::failure(
                counted.error());
          }
        }
        continue;
      }
      if (entry.kind != PathObjectKind::directory) {
        observation.ambiguous_object_detected = true;
        continue;
      }
      const clonecore::Status counted = increment_counter(
          observation.top_level_non_microsoft_namespace_count);
      if (!counted) {
        return clonecore::Result<EfiBootOwnershipEvidence>::failure(
            counted.error());
      }
    }
    return classify_efi_boot_ownership(observation);
  }

 private:
  std::unique_ptr<IExecutableTrustVerifier> trust_verifier_;
};

}  // namespace

clonecore::Result<EfiBootOwnershipEvidence>
classify_efi_boot_ownership(
    const EfiBootOwnershipObservation& observation) {
  const std::uint64_t non_microsoft_count =
      static_cast<std::uint64_t>(
          observation.top_level_non_microsoft_namespace_count) +
      static_cast<std::uint64_t>(
          observation.boot_namespace_nonstandard_entries) +
      static_cast<std::uint64_t>(
          observation.non_microsoft_or_untrusted_efi_loader_count);
  if (non_microsoft_count >
      (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Result<EfiBootOwnershipEvidence>::failure(
        ownership_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"ESP所有権項目数の合計",
            L"ESP所有権の項目数が表現上限を超えました"));
  }

  EfiBootOwnershipState state =
      EfiBootOwnershipState::microsoft_only_or_empty;
  if (observation.ambiguous_object_detected) {
    state = EfiBootOwnershipState::ambiguous;
  } else if (non_microsoft_count != 0U ||
             (observation.fallback_loader_present &&
              !observation.fallback_loader_microsoft_signed)) {
    state = EfiBootOwnershipState::non_microsoft_or_untrusted_present;
  }
  return clonecore::Result<EfiBootOwnershipEvidence>::success(
      EfiBootOwnershipEvidence{
          .state = state,
          .efi_directory_present = observation.efi_directory_present,
          .microsoft_namespace_present =
              observation.microsoft_namespace_present,
          .boot_namespace_present = observation.boot_namespace_present,
          .fallback_loader_present = observation.fallback_loader_present,
          .fallback_loader_microsoft_signed =
              observation.fallback_loader_microsoft_signed,
          .microsoft_signed_efi_loader_count =
              observation.microsoft_signed_efi_loader_count,
          .non_microsoft_or_untrusted_entry_count =
              static_cast<std::uint32_t>(non_microsoft_count),
          .top_level_non_microsoft_namespace_count =
              observation.top_level_non_microsoft_namespace_count,
          .boot_namespace_nonstandard_entry_count =
              observation.boot_namespace_nonstandard_entries,
          .non_microsoft_or_untrusted_efi_loader_count =
              observation.non_microsoft_or_untrusted_efi_loader_count,
      });
}

bool equivalent_efi_boot_ownership(
    const EfiBootOwnershipEvidence& left,
    const EfiBootOwnershipEvidence& right) noexcept {
  return left.state == right.state &&
      left.efi_directory_present == right.efi_directory_present &&
      left.microsoft_namespace_present ==
          right.microsoft_namespace_present &&
      left.boot_namespace_present == right.boot_namespace_present &&
      left.fallback_loader_present == right.fallback_loader_present &&
      left.fallback_loader_microsoft_signed ==
          right.fallback_loader_microsoft_signed &&
      left.microsoft_signed_efi_loader_count ==
          right.microsoft_signed_efi_loader_count &&
      left.non_microsoft_or_untrusted_entry_count ==
          right.non_microsoft_or_untrusted_entry_count &&
      left.top_level_non_microsoft_namespace_count ==
          right.top_level_non_microsoft_namespace_count &&
      left.boot_namespace_nonstandard_entry_count ==
          right.boot_namespace_nonstandard_entry_count &&
      left.non_microsoft_or_untrusted_efi_loader_count ==
          right.non_microsoft_or_untrusted_efi_loader_count;
}

bool efi_boot_ownership_allows_microsoft_rebuild(
    const EfiBootOwnershipEvidence& evidence) noexcept {
  return evidence.state ==
      EfiBootOwnershipState::microsoft_only_or_empty &&
      evidence.non_microsoft_or_untrusted_entry_count == 0U &&
      (!evidence.fallback_loader_present ||
       evidence.fallback_loader_microsoft_signed);
}

bool efi_boot_ownership_allows_third_party_preserve(
    const EfiBootOwnershipEvidence& evidence) noexcept {
  return evidence.state ==
          EfiBootOwnershipState::non_microsoft_or_untrusted_present &&
      evidence.efi_directory_present &&
      evidence.top_level_non_microsoft_namespace_count != 0U &&
      evidence.boot_namespace_nonstandard_entry_count == 0U &&
      evidence.non_microsoft_or_untrusted_efi_loader_count == 0U &&
      evidence.non_microsoft_or_untrusted_entry_count ==
          evidence.top_level_non_microsoft_namespace_count &&
      (!evidence.fallback_loader_present ||
       evidence.fallback_loader_microsoft_signed);
}

std::unique_ptr<IEfiBootOwnershipInspector>
make_windows_efi_boot_ownership_inspector(
    std::unique_ptr<IExecutableTrustVerifier> trust_verifier) {
  if (trust_verifier == nullptr) {
    return nullptr;
  }
  return std::make_unique<WindowsEfiBootOwnershipInspector>(
      std::move(trust_verifier));
}

std::unique_ptr<IEfiBootOwnershipInspector>
make_windows_efi_boot_ownership_inspector() {
  return make_windows_efi_boot_ownership_inspector(
      make_windows_authenticode_verifier());
}

}  // namespace ytec::bootrepair

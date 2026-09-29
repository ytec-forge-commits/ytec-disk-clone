#include "ytec/bootrepair/winre_diagnostic.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::Status;
using clonecore::UniqueHandle;

constexpr std::uint64_t kMaximumWinReImageBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kWinReDiagnosticTimeoutMilliseconds =
    2U * 60U * 1000U;

clonecore::Error winre_error(
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

bool is_drive_absolute_path(const std::wstring& path) {
  return path.size() >= 3U &&
         ((path[0] >= L'A' && path[0] <= L'Z') ||
          (path[0] >= L'a' && path[0] <= L'z')) &&
         path[1] == L':' &&
         (path[2] == L'\\' || path[2] == L'/');
}

bool has_forbidden_path_text(const std::wstring& path) {
  if (path.find(L'"') != std::wstring::npos ||
      path.find(L'\r') != std::wstring::npos ||
      path.find(L'\n') != std::wstring::npos ||
      path.find(L'*') != std::wstring::npos ||
      path.find(L'?') != std::wstring::npos) {
    return true;
  }
  std::wstring normalized = path;
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  return normalized.find(L"\\..\\") != std::wstring::npos ||
         normalized.ends_with(L"\\..") ||
         normalized.starts_with(L"..\\");
}

std::wstring normalize_directory(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3U && value.back() == L'\\') {
    value.pop_back();
  }
  return value;
}

bool ends_with_case_insensitive(
    const std::wstring& value,
    const std::wstring_view suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  const std::wstring_view ending(
      value.data() + value.size() - suffix.size(), suffix.size());
  return _wcsnicmp(
             ending.data(), suffix.data(), suffix.size()) == 0;
}

Status validate_offline_windows_directory(
    const std::wstring& directory) {
  const std::wstring normalized = normalize_directory(directory);
  if (!is_drive_absolute_path(normalized) ||
      has_forbidden_path_text(normalized) ||
      !ends_with_case_insensitive(normalized, L"\\Windows")) {
    return Status::failure(winre_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinREオフラインWindowsパス",
        L"ドライブ絶対パスで終端がWindowsのディレクトリだけを指定できます"));
  }
  return clonecore::success_status();
}

Status validate_trusted_system_directory(
    const std::wstring& directory) {
  const std::wstring normalized = normalize_directory(directory);
  if (!is_drive_absolute_path(normalized) ||
      has_forbidden_path_text(normalized) ||
      !ends_with_case_insensitive(normalized, L"\\Windows\\System32")) {
    return Status::failure(winre_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinRE診断System32パス",
        L"現在のWindowsまたはWinPEのSystem32絶対パスが必要です"));
  }
  return clonecore::success_status();
}

Result<std::uint32_t> parse_decimal(
    const std::string_view text,
    const std::size_t begin,
    const std::size_t end,
    const std::wstring_view description) {
  if (begin >= end) {
    return Result<std::uint32_t>::failure(winre_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(description),
        L"REAgentCの登録先番号がありません"));
  }
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(
      text.data() + begin, text.data() + end, value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + end) {
    return Result<std::uint32_t>::failure(winre_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(description),
        L"REAgentCの登録先番号が32ビット範囲の10進数ではありません"));
  }
  return Result<std::uint32_t>::success(value);
}

std::wstring make_registered_winre_image_path(
    const WinReRegisteredLocation& location) {
  const std::wstring relative = location.path_kind ==
          WinReRegisteredPathKind::windows_system32_recovery
      ? L"\\Windows\\System32\\Recovery\\Winre.wim"
      : L"\\Recovery\\WindowsRE\\Winre.wim";
  return L"\\\\?\\GLOBALROOT\\device\\harddisk" +
         std::to_wstring(location.disk_number) + L"\\partition" +
         std::to_wstring(location.partition_number) + relative;
}

class WindowsWinReImageProbe final : public IWinReImageProbe {
 public:
  Result<WinReImageObservation> inspect_regular_image(
      const std::wstring& path) override {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return Result<WinReImageObservation>::success(
            WinReImageObservation{});
      }
      return Result<WinReImageObservation>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              L"WinREイメージ属性取得",
              native_code));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return Result<WinReImageObservation>::failure(winre_error(
          ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"WinREイメージ通常ファイル検証",
          L"Winre.wimが通常ファイルではないかreparse pointです"));
    }

    HANDLE raw = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      return Result<WinReImageObservation>::failure(
          clonecore::make_win32_error(
              ErrorCode::io_failed,
              L"WinREイメージ読取り専用オープン",
              GetLastError()));
    }
    UniqueHandle file(raw);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
      return Result<WinReImageObservation>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              L"WinREイメージ容量取得",
              GetLastError()));
    }
    if (size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) >
            kMaximumWinReImageBytes) {
      return Result<WinReImageObservation>::failure(winre_error(
          ErrorCode::invalid_data,
          ERROR_FILE_INVALID,
          L"WinREイメージ容量検証",
          L"Winre.wimが空か安全な診断上限を超えています"));
    }
    return Result<WinReImageObservation>::success(
        WinReImageObservation{
            .exists = true,
            .length =
                static_cast<std::uint64_t>(size.QuadPart),
        });
  }
};

class WindowsWinReDiagnosticService final
    : public IWinReDiagnosticService {
 public:
  Result<WinReDiagnosticReport> inspect(
      const std::wstring& offline_windows_directory,
      const std::uint32_t expected_target_disk_number) override {
    constexpr std::size_t kMaximumSystemDirectoryCharacters = 32768U;
    std::vector<wchar_t> system_directory(
        kMaximumSystemDirectoryCharacters);
    const UINT length = GetSystemDirectoryW(
        system_directory.data(),
        static_cast<UINT>(system_directory.size()));
    if (length == 0U) {
      return Result<WinReDiagnosticReport>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              L"WinRE診断System32取得",
              GetLastError()));
    }
    if (length >= system_directory.size()) {
      return Result<WinReDiagnosticReport>::failure(winre_error(
          ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"WinRE診断System32長",
          L"現在のSystem32パスが安全上限を超えています"));
    }
    return inspect_winre_source_with_windows_apis(
        WinReDiagnosticRequest{
            .offline_windows_directory =
                offline_windows_directory,
            .trusted_system_directory =
                std::wstring(system_directory.data(), length),
            .expected_target_disk_number =
                expected_target_disk_number,
        });
  }
};

}  // namespace

Result<std::optional<WinReRegisteredLocation>>
parse_reagentc_registered_location(
    const std::string_view standard_output) {
  std::string normalized;
  normalized.reserve(standard_output.size());
  for (const unsigned char byte : standard_output) {
    if (byte == 0U) {
      continue;
    }
    const char value = static_cast<char>(byte);
    normalized.push_back(
        value >= 'A' && value <= 'Z'
            ? static_cast<char>(value - ('A' - 'a'))
            : value);
  }

  constexpr std::string_view kPrefix =
      R"(\\?\globalroot\device\harddisk)";
  constexpr std::string_view kPartition = R"(\partition)";
  constexpr std::string_view kRecovery =
      R"(\recovery\windowsre)";
  constexpr std::string_view kWindowsFallback =
      R"(\windows\system32\recovery)";
  std::optional<WinReRegisteredLocation> observed;
  std::size_t search_from = 0;
  for (;;) {
    const std::size_t prefix =
        normalized.find(kPrefix, search_from);
    if (prefix == std::string::npos) {
      break;
    }
    std::size_t cursor = prefix + kPrefix.size();
    const std::size_t disk_begin = cursor;
    while (cursor < normalized.size() &&
           normalized[cursor] >= '0' &&
           normalized[cursor] <= '9') {
      ++cursor;
    }
    const auto disk = parse_decimal(
        normalized,
        disk_begin,
        cursor,
        L"REAgentC登録先ディスク番号");
    if (!disk) {
      return Result<std::optional<WinReRegisteredLocation>>::
          failure(disk.error());
    }
    if (normalized.compare(cursor, kPartition.size(), kPartition) !=
        0) {
      return Result<std::optional<WinReRegisteredLocation>>::
          failure(winre_error(
              ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"REAgentC登録先パーティション",
              L"GLOBALROOT登録先にpartition番号がありません"));
    }
    cursor += kPartition.size();
    const std::size_t partition_begin = cursor;
    while (cursor < normalized.size() &&
           normalized[cursor] >= '0' &&
           normalized[cursor] <= '9') {
      ++cursor;
    }
    const auto partition = parse_decimal(
        normalized,
        partition_begin,
        cursor,
        L"REAgentC登録先パーティション番号");
    if (!partition) {
      return Result<std::optional<WinReRegisteredLocation>>::
          failure(partition.error());
    }
    WinReRegisteredPathKind path_kind{};
    std::size_t path_length = 0U;
    if (normalized.compare(cursor, kRecovery.size(), kRecovery) == 0) {
      path_kind = WinReRegisteredPathKind::recovery_windows_re;
      path_length = kRecovery.size();
    } else if (normalized.compare(
                   cursor, kWindowsFallback.size(), kWindowsFallback) ==
               0) {
      path_kind =
          WinReRegisteredPathKind::windows_system32_recovery;
      path_length = kWindowsFallback.size();
    }
    if (partition.value() == 0U || path_length == 0U) {
      return Result<std::optional<WinReRegisteredLocation>>::
          failure(winre_error(
              ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"REAgentC登録先Recoveryパス",
              L"登録先が対応するRecovery\\WindowsREまたは"
              L"Windows\\System32\\Recoveryパスではありません"));
    }
    const WinReRegisteredLocation candidate{
        .disk_number = disk.value(),
        .partition_number = partition.value(),
        .path_kind = path_kind,
    };
    if (observed.has_value() &&
        (observed->disk_number != candidate.disk_number ||
         observed->partition_number !=
             candidate.partition_number ||
         observed->path_kind != candidate.path_kind)) {
      return Result<std::optional<WinReRegisteredLocation>>::
          failure(winre_error(
              ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              L"REAgentC登録先一意性",
              L"異なる複数のWinRE登録先が報告されました"));
    }
    observed = candidate;
    search_from = cursor + path_length;
  }
  return Result<std::optional<WinReRegisteredLocation>>::success(
      observed);
}

Result<std::vector<std::wstring>> build_reagentc_info_arguments(
    const std::wstring& offline_windows_directory) {
  const Status valid =
      validate_offline_windows_directory(offline_windows_directory);
  if (!valid) {
    return Result<std::vector<std::wstring>>::failure(
        valid.error());
  }
  return Result<std::vector<std::wstring>>::success({
      L"/info",
      L"/target",
      normalize_directory(offline_windows_directory),
  });
}

Result<WinReDiagnosticReport> inspect_winre_source(
    const WinReDiagnosticRequest& request,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IWinReImageProbe& image_probe) {
  const auto arguments = build_reagentc_info_arguments(
      request.offline_windows_directory);
  if (!arguments) {
    return Result<WinReDiagnosticReport>::failure(
        arguments.error());
  }
  const Status system_directory =
      validate_trusted_system_directory(
          request.trusted_system_directory);
  if (!system_directory) {
    return Result<WinReDiagnosticReport>::failure(
        system_directory.error());
  }

  const std::wstring trusted_directory =
      normalize_directory(request.trusted_system_directory);
  const std::wstring executable_path =
      trusted_directory + L"\\reagentc.exe";
  const Status trusted =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!trusted) {
    return Result<WinReDiagnosticReport>::failure(
        trusted.error());
  }
  const auto process = process_runner.run(
      executable_path, arguments.value(), trusted_directory);
  if (!process) {
    return Result<WinReDiagnosticReport>::failure(
        process.error());
  }

  WinReDiagnosticReport report{
      .executable_path = executable_path,
      .exit_code = process.value().exit_code,
      .standard_output = process.value().standard_output,
      .standard_error = process.value().standard_error,
      .microsoft_signature_verified = true,
      .read_only_command = true,
  };
  if (process.value().exit_code != 0U) {
    return Result<WinReDiagnosticReport>::success(
        std::move(report));
  }

  const auto registered = parse_reagentc_registered_location(
      process.value().standard_output);
  if (!registered) {
    return Result<WinReDiagnosticReport>::failure(
        registered.error());
  }

  const std::wstring offline_windows =
      normalize_directory(request.offline_windows_directory);
  const std::wstring fallback =
      offline_windows + L"\\System32\\Recovery\\Winre.wim";
  if (registered.value().has_value()) {
    report.registered_location_reported = true;
    report.registered_path_kind_reported = true;
    report.registered_path_kind = registered.value()->path_kind;
    if (registered.value()->disk_number !=
        request.expected_target_disk_number) {
      if (request.
              allow_mismatched_registered_location_as_cloned_source_stale) {
        // The foreign disk path must never be opened. A later reviewed direct
        // clone transaction may replace this copied source-machine record,
        // but generic diagnosis and rebuild planning keep the flag false.
        report.source_state = WinReSourceState::registered_partition;
        report.registered_partition_number =
            registered.value()->partition_number;
        report.
            registered_location_mismatch_classified_as_cloned_source_stale =
                true;
        return Result<WinReDiagnosticReport>::success(
            std::move(report));
      }
      return Result<WinReDiagnosticReport>::failure(winre_error(
          ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"WinRE登録先ディスク照合",
          L"REAgentCの登録先が再識別した変換対象ディスクと一致しません"));
    }
    report.registered_location_matches_expected_disk = true;
    report.registered_partition_number =
        registered.value()->partition_number;
    report.inspected_image_path =
        make_registered_winre_image_path(*registered.value());
    const auto registered_image =
        image_probe.inspect_regular_image(
            report.inspected_image_path);
    if (!registered_image) {
      return Result<WinReDiagnosticReport>::failure(
          registered_image.error());
    }
    if (registered_image.value().exists) {
      report.source_state =
          WinReSourceState::registered_partition;
      report.registered_image_present = true;
      report.winre_image_size_bytes =
          registered_image.value().length;
      return Result<WinReDiagnosticReport>::success(
          std::move(report));
    }
  }

  report.inspected_image_path = fallback;
  const auto fallback_image =
      image_probe.inspect_regular_image(fallback);
  if (!fallback_image) {
    return Result<WinReDiagnosticReport>::failure(
        fallback_image.error());
  }
  if (fallback_image.value().exists) {
    report.source_state =
        WinReSourceState::image_available_in_windows;
    report.fallback_image_present = true;
    report.winre_image_size_bytes =
        fallback_image.value().length;
  } else {
    report.source_state =
        report.registered_location_reported
            ? WinReSourceState::unknown
            : WinReSourceState::missing;
  }
  return Result<WinReDiagnosticReport>::success(
      std::move(report));
}

Status apply_winre_diagnostic_to_rebuild_request(
    const WinReDiagnosticReport& report,
    Mbr2GptRebuildRequest& request) {
  if (!report.microsoft_signature_verified ||
      !report.read_only_command ||
      report.exit_code != 0U ||
      report.source_state == WinReSourceState::unknown ||
      (report.source_state != WinReSourceState::missing &&
       report.winre_image_size_bytes == 0U)) {
    return Status::failure(winre_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"WinRE診断の再構築計画入力",
        L"署名済み読取り専用診断でWinREイメージを確認できません"));
  }
  if (report.source_state ==
          WinReSourceState::registered_partition &&
      (!report.registered_location_reported ||
       !report.registered_location_matches_expected_disk ||
       !report.registered_image_present ||
       report.registered_partition_number == 0U)) {
    return Status::failure(winre_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"WinRE登録区画の再構築計画入力",
        L"登録区画、対象ディスク、Winre.wimの照合が完了していません"));
  }
  if (report.source_state ==
          WinReSourceState::image_available_in_windows &&
      !report.fallback_image_present) {
    return Status::failure(winre_error(
        ErrorCode::verification_failed,
        ERROR_FILE_NOT_FOUND,
        L"WinREフォールバックの再構築計画入力",
        L"Windows内のWinre.wimを読取り確認できません"));
  }

  request.winre_state = report.source_state;
  request.registered_winre_partition_number =
      report.source_state ==
              WinReSourceState::registered_partition
          ? report.registered_partition_number
          : 0U;
  request.winre_image_size_bytes =
      report.source_state == WinReSourceState::missing
          ? 0U
          : report.winre_image_size_bytes;
  return clonecore::success_status();
}

std::unique_ptr<IWinReImageProbe>
make_windows_winre_image_probe() {
  return std::make_unique<WindowsWinReImageProbe>();
}

Result<WinReDiagnosticReport>
inspect_winre_source_with_windows_apis(
    const WinReDiagnosticRequest& request) {
  auto trust = make_windows_authenticode_verifier();
  auto runner = make_windows_process_runner(
      kWinReDiagnosticTimeoutMilliseconds);
  auto probe = make_windows_winre_image_probe();
  return inspect_winre_source(
      request, *trust, *runner, *probe);
}

std::unique_ptr<IWinReDiagnosticService>
make_windows_winre_diagnostic_service() {
  return std::make_unique<WindowsWinReDiagnosticService>();
}

}  // namespace ytec::bootrepair

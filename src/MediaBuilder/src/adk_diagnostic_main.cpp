#include "ytec/mediabuilder/adk_detection.h"

#include <Windows.h>

#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string to_utf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return "<path-too-long>";
  }

  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return "<utf8-conversion-failed>";
  }

  std::string output(static_cast<std::size_t>(required), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      output.data(),
      required,
      nullptr,
      nullptr);
  if (written != required) {
    return "<utf8-conversion-failed>";
  }
  return output;
}

std::string path_to_utf8(const std::filesystem::path& path) {
  return to_utf8(path.native());
}

std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20U) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned int>(character) << std::dec;
        } else {
          output << static_cast<char>(character);
        }
        break;
    }
  }
  return output.str();
}

const char* json_bool(bool value) noexcept {
  return value ? "true" : "false";
}

void print_json(const ytec::mediabuilder::AdkDiscoveryReport& report) {
  std::cout << "{\n"
            << "  \"schemaVersion\": 1,\n"
            << "  \"architecture\": \""
            << json_escape(to_utf8(report.architecture)) << "\",\n"
            << "  \"baseLayoutReady\": "
            << json_bool(report.base_layout_ready()) << ",\n"
            << "  \"bootexLayoutReady\": "
            << json_bool(report.bootex_layout_ready()) << ",\n"
            << "  \"mediaCreationPermitted\": "
            << json_bool(report.media_creation_permitted()) << ",\n"
            << "  \"selectedCandidateIndex\": ";
  if (report.selected_candidate_index) {
    std::cout << *report.selected_candidate_index;
  } else {
    std::cout << "null";
  }
  std::cout << ",\n  \"candidates\": [\n";

  for (std::size_t index = 0; index < report.candidates.size(); ++index) {
    const auto& candidate = report.candidates[index];
    std::cout << "    {\n"
              << "      \"root\": \""
              << json_escape(path_to_utf8(candidate.root)) << "\",\n"
              << "      \"deploymentToolsPresent\": "
              << json_bool(candidate.deployment_tools_present) << ",\n"
              << "      \"winpeAddonPresent\": "
              << json_bool(candidate.winpe_addon_present) << ",\n"
              << "      \"deploymentToolsVersion\": \""
              << json_escape(to_utf8(candidate.deployment_tools_version))
              << "\",\n"
              << "      \"dismFileVersion\": \""
              << json_escape(to_utf8(
                     ytec::mediabuilder::format_file_version(
                         candidate.dism_file_version)))
              << "\",\n"
              << "      \"microsoftToolsTrusted\": "
              << json_bool(candidate.microsoft_tools_trusted) << ",\n"
              << "      \"bootexSupported\": "
              << json_bool(candidate.bootex_supported) << ",\n"
              << "      \"baseLayoutReady\": "
              << json_bool(candidate.base_layout_ready) << ",\n"
              << "      \"bootexLayoutReady\": "
              << json_bool(candidate.bootex_layout_ready) << ",\n"
              << "      \"oscdimgServicingPatchApplied\": "
              << json_bool(candidate.oscdimg_servicing_patch_applied)
              << ",\n"
              << "      \"dismServicingPatchApplied\": "
              << json_bool(candidate.dism_servicing_patch_applied)
              << ",\n"
              << "      \"versionAndServicingVerified\": "
              << json_bool(candidate.version_and_servicing_verified) << ",\n"
              << "      \"mediaCreationPermitted\": "
              << json_bool(candidate.media_creation_permitted) << ",\n"
              << "      \"diagnostics\": [\n";

    for (std::size_t diagnostic_index = 0;
         diagnostic_index < candidate.diagnostics.size();
         ++diagnostic_index) {
      const auto& diagnostic = candidate.diagnostics[diagnostic_index];
      std::cout << "        {\"severity\": \""
                << json_escape(to_utf8(
                       ytec::mediabuilder::diagnostic_severity_name(
                           diagnostic.severity)))
                << "\", \"code\": \"" << json_escape(diagnostic.code)
                << "\", \"path\": \""
                << json_escape(path_to_utf8(diagnostic.path))
                << "\", \"message\": \""
                << json_escape(to_utf8(diagnostic.message))
                << "\", \"nativeCode\": " << diagnostic.native_code
                << "}";
      if (diagnostic_index + 1U < candidate.diagnostics.size()) {
        std::cout << ',';
      }
      std::cout << '\n';
    }

    std::cout << "      ]\n    }";
    if (index + 1U < report.candidates.size()) {
      std::cout << ',';
    }
    std::cout << '\n';
  }

  std::cout << "  ]\n}\n";
}

void print_text(const ytec::mediabuilder::AdkDiscoveryReport& report) {
  std::cout << to_utf8(L"Y-TEC WinPE環境診断（読み取り専用）\n")
            << to_utf8(L"アーキテクチャ: ")
            << to_utf8(report.architecture) << '\n'
            << to_utf8(L"基本WinPEローカル構成: ")
            << (report.base_layout_ready() ? "PASS" : "NOT READY")
            << '\n'
            << to_utf8(L"/bootex対応ローカル構成: ")
            << (report.bootex_layout_ready() ? "PASS" : "NOT READY")
            << '\n'
            << to_utf8(L"メディア作成許可: ")
            << (report.media_creation_permitted() ? "PASS" : "NOT READY")
            << "\n\n";

  for (std::size_t index = 0; index < report.candidates.size(); ++index) {
    const auto& candidate = report.candidates[index];
    std::cout << to_utf8(L"候補 ") << (index + 1U) << ": "
              << path_to_utf8(candidate.root) << '\n'
              << "  " << to_utf8(L"Deployment Toolsバージョン: ")
              << to_utf8(candidate.deployment_tools_version) << '\n'
              << "  " << to_utf8(L"DISMファイルバージョン: ")
              << to_utf8(ytec::mediabuilder::format_file_version(
                     candidate.dism_file_version))
              << '\n'
              << "  " << to_utf8(L"KB5101684 Oscdimg更新: ")
              << (candidate.oscdimg_servicing_patch_applied ? "PASS"
                                                            : "NOT READY")
              << '\n'
              << "  " << to_utf8(L"KB5101684 DISM更新: ")
              << (candidate.dism_servicing_patch_applied ? "PASS"
                                                         : "NOT READY")
              << '\n';
    for (const auto& diagnostic : candidate.diagnostics) {
      std::cout << "  ["
                << to_utf8(ytec::mediabuilder::diagnostic_severity_name(
                       diagnostic.severity))
                << "] " << diagnostic.code << ": "
                << to_utf8(diagnostic.message);
      if (!diagnostic.path.empty()) {
        std::cout << " (" << path_to_utf8(diagnostic.path) << ')';
      }
      std::cout << '\n';
    }
  }

  if (!report.media_creation_permitted()) {
    std::cout
        << '\n'
        << to_utf8(
               L"ADK/WinPE Add-onの自動取得やインストールは行いません。Microsoft公式手順で導入後、再診断してください。\n");
  }
}

}  // namespace

int wmain(int argument_count, wchar_t* arguments[]) {
  SetConsoleOutputCP(CP_UTF8);

  bool json = false;
  if (argument_count > 2) {
    std::cerr << "Usage: ytec-winpe-environment.exe [--text|--json]\n";
    return 64;
  }
  if (argument_count == 2) {
    const std::wstring_view argument(arguments[1]);
    if (argument == L"--json") {
      json = true;
    } else if (argument != L"--text") {
      std::cerr << "Usage: ytec-winpe-environment.exe [--text|--json]\n";
      return 64;
    }
  }

  auto environment = ytec::mediabuilder::make_windows_adk_environment();
  const auto report = ytec::mediabuilder::detect_windows_adk(*environment);
  if (json) {
    print_json(report);
  } else {
    print_text(report);
  }
  return report.media_creation_permitted() ? 0 : 2;
}

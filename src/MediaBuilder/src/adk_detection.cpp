#include "ytec/mediabuilder/adk_detection.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace ytec::mediabuilder {
namespace {

constexpr std::size_t kMaximumCommandScriptBytes = 1024U * 1024U;
constexpr wchar_t kRequiredDeploymentToolsProductCode[] =
    L"{7C2ACA41-3E86-6C9C-D31E-E893512BD9BA}";
constexpr wchar_t kRequiredDeploymentToolsVersion[] = L"10.1.26100.2454";
constexpr FileVersion kRequiredServicedDismVersion{10U, 0U, 26100U, 8972U};
constexpr wchar_t kRequiredOscdimgProductCode[] =
    L"{AA0852D8-D1C3-5D2E-34B2-282A4F10036E}";
constexpr wchar_t kRequiredOscdimgPatchCode[] =
    L"{83449E02-24CA-44C1-A0A3-80A9AA2E85F0}";
constexpr wchar_t kRequiredDismProductCode[] =
    L"{765CB4D6-1A08-1F46-81D0-7016DA3604B2}";
constexpr wchar_t kRequiredDismPatchCode[] =
    L"{BF926991-C615-45A6-BF73-AFC83E790865}";

void add_diagnostic(
    AdkCandidateReport& report,
    DiagnosticSeverity severity,
    std::string code,
    const std::filesystem::path& path,
    std::wstring message,
    std::uint32_t native_code = ERROR_SUCCESS) {
  report.diagnostics.push_back(AdkDiagnostic{
      severity,
      std::move(code),
      path,
      std::move(message),
      native_code,
  });
}

bool require_path_kind(
    AdkCandidateReport& report,
    IAdkEnvironment& environment,
    const std::filesystem::path& path,
    PathKind expected_kind,
    std::string missing_code,
    std::wstring missing_message) {
  const auto result = environment.classify_path(path);
  if (!result) {
    add_diagnostic(
        report,
        DiagnosticSeverity::error,
        "ADK_PATH_PROBE_FAILED",
        path,
        L"ADK構成要素を安全に照会できませんでした。",
        result.error().native_code);
    return false;
  }

  const PathKind actual_kind = result.value();
  if (actual_kind == PathKind::reparse_point) {
    add_diagnostic(
        report,
        DiagnosticSeverity::error,
        "ADK_REPARSE_POINT_REJECTED",
        path,
        L"必要なADK構成要素がreparse pointのため、安全側に停止します。",
        ERROR_REPARSE_TAG_INVALID);
    return false;
  }

  if (actual_kind != expected_kind) {
    add_diagnostic(
        report,
        DiagnosticSeverity::error,
        std::move(missing_code),
        path,
        std::move(missing_message),
        actual_kind == PathKind::missing ? ERROR_FILE_NOT_FOUND
                                         : ERROR_INVALID_DATA);
    return false;
  }

  return true;
}

bool verify_microsoft_tool(
    AdkCandidateReport& report,
    IAdkEnvironment& environment,
    const std::filesystem::path& path) {
  const auto status = environment.verify_microsoft_signed_executable(path);
  if (status) {
    return true;
  }

  add_diagnostic(
      report,
      DiagnosticSeverity::error,
      "ADK_MICROSOFT_SIGNATURE_REQUIRED",
      path,
      L"ADK実行ファイルのMicrosoft署名を検証できませんでした。",
      status.error().native_code);
  return false;
}

bool contains_ascii_case_insensitive(
    std::string_view haystack,
    std::string_view needle) {
  if (needle.empty()) {
    return true;
  }

  const auto lower = [](char value) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(value)));
  };

  return std::search(
             haystack.begin(),
             haystack.end(),
             needle.begin(),
             needle.end(),
             [lower](char left, char right) {
               return lower(left) == lower(right);
             }) != haystack.end();
}

bool verify_servicing_patch(
    AdkCandidateReport& report,
    IAdkEnvironment& environment,
    std::wstring_view product_code,
    std::wstring_view patch_code,
    bool& applied,
    std::string missing_code,
    std::wstring missing_message) {
  const auto result =
      environment.is_msi_patch_applied(product_code, patch_code);
  if (!result) {
    add_diagnostic(
        report,
        DiagnosticSeverity::error,
        "ADK_SERVICING_QUERY_FAILED",
        report.root,
        L"Windows InstallerからADK更新適用状態を読み取れませんでした。",
        result.error().native_code);
    applied = false;
    return false;
  }

  applied = result.value();
  if (!applied) {
    add_diagnostic(
        report,
        DiagnosticSeverity::error,
        std::move(missing_code),
        report.root,
        std::move(missing_message),
        ERROR_REVISION_MISMATCH);
  }
  return applied;
}

}  // namespace

bool AdkDiscoveryReport::base_layout_ready() const noexcept {
  if (!selected_candidate_index ||
      *selected_candidate_index >= candidates.size()) {
    return false;
  }
  return candidates[*selected_candidate_index].base_layout_ready;
}

bool AdkDiscoveryReport::bootex_layout_ready() const noexcept {
  if (!selected_candidate_index ||
      *selected_candidate_index >= candidates.size()) {
    return false;
  }
  return candidates[*selected_candidate_index].bootex_layout_ready;
}

bool AdkDiscoveryReport::media_creation_permitted() const noexcept {
  if (!selected_candidate_index ||
      *selected_candidate_index >= candidates.size()) {
    return false;
  }
  return candidates[*selected_candidate_index].media_creation_permitted;
}

AdkCandidateReport inspect_adk_candidate(
    const std::filesystem::path& root,
    std::wstring_view architecture,
    IAdkEnvironment& environment) {
  AdkCandidateReport report{};
  report.root = root;
  report.architecture = std::wstring(architecture);
  report.deployment_tools_root = root / L"Deployment Tools";
  report.winpe_root = root / L"Windows Preinstallation Environment";
  report.copype_path = report.winpe_root / L"copype.cmd";
  report.make_winpe_media_path =
      report.winpe_root / L"MakeWinPEMedia.cmd";
  report.dism_path = report.deployment_tools_root / report.architecture /
                     L"DISM" / L"dism.exe";
  report.oscdimg_path = report.deployment_tools_root / report.architecture /
                        L"Oscdimg" / L"oscdimg.exe";
  report.base_winpe_wim_path = report.winpe_root / report.architecture /
                               L"en-us" / L"winpe.wim";

  if (report.architecture != L"amd64") {
    add_diagnostic(
        report,
        DiagnosticSeverity::error,
        "ADK_ARCHITECTURE_UNSUPPORTED",
        root,
        L"現在の製品境界ではWinPE amd64だけを許可します。",
        ERROR_NOT_SUPPORTED);
    return report;
  }

  if (!require_path_kind(
          report,
          environment,
          report.root,
          PathKind::directory,
          "ADK_ROOT_NOT_FOUND",
          L"Windows ADKのインストール先が見つかりません。")) {
    return report;
  }

  const bool deployment_root_ok = require_path_kind(
      report,
      environment,
      report.deployment_tools_root,
      PathKind::directory,
      "ADK_DEPLOYMENT_TOOLS_MISSING",
      L"ADK Deployment Toolsが見つかりません。");
  const bool winpe_root_ok = require_path_kind(
      report,
      environment,
      report.winpe_root,
      PathKind::directory,
      "ADK_WINPE_ADDON_MISSING",
      L"Windows PE Add-onが見つかりません。ADKとは別に導入が必要です。");

  bool deployment_files_ok = false;
  bool microsoft_tools_trusted = false;
  if (deployment_root_ok) {
    const bool dism_ok = require_path_kind(
        report,
        environment,
        report.dism_path,
        PathKind::regular_file,
        "ADK_DISM_MISSING",
        L"amd64版DISMが見つかりません。");
    const bool oscdimg_ok = require_path_kind(
        report,
        environment,
        report.oscdimg_path,
        PathKind::regular_file,
        "ADK_OSCDIMG_MISSING",
        L"amd64版Oscdimgが見つかりません。");
    deployment_files_ok = dism_ok && oscdimg_ok;

    if (deployment_files_ok) {
      const bool dism_trusted =
          verify_microsoft_tool(report, environment, report.dism_path);
      const bool oscdimg_trusted =
          verify_microsoft_tool(report, environment, report.oscdimg_path);
      microsoft_tools_trusted = dism_trusted && oscdimg_trusted;
    }
  }

  bool winpe_files_ok = false;
  bool script_readable = false;
  if (winpe_root_ok) {
    const bool copype_ok = require_path_kind(
        report,
        environment,
        report.copype_path,
        PathKind::regular_file,
        "ADK_COPYPE_MISSING",
        L"copype.cmdが見つかりません。");
    const bool make_media_ok = require_path_kind(
        report,
        environment,
        report.make_winpe_media_path,
        PathKind::regular_file,
        "ADK_MAKEWINPEMEDIA_MISSING",
        L"MakeWinPEMedia.cmdが見つかりません。");
    const bool wim_ok = require_path_kind(
        report,
        environment,
        report.base_winpe_wim_path,
        PathKind::regular_file,
        "ADK_BASE_WINPE_WIM_MISSING",
        L"amd64版の基本winpe.wimが見つかりません。");
    winpe_files_ok = copype_ok && make_media_ok && wim_ok;

    if (make_media_ok) {
      const auto contents = environment.read_text_file(
          report.make_winpe_media_path, kMaximumCommandScriptBytes);
      if (!contents) {
        add_diagnostic(
            report,
            DiagnosticSeverity::error,
            "ADK_MAKEWINPEMEDIA_READ_FAILED",
            report.make_winpe_media_path,
            L"MakeWinPEMedia.cmdを上限付きで読み取れませんでした。",
            contents.error().native_code);
      } else {
        script_readable = true;
        report.bootex_supported =
            contains_ascii_case_insensitive(contents.value(), "/bootex");
        if (!report.bootex_supported) {
          add_diagnostic(
              report,
              DiagnosticSeverity::warning,
              "ADK_BOOTEX_NOT_SUPPORTED",
              report.make_winpe_media_path,
              L"/bootexを確認できないため、Windows UEFI 2023 CA用メディアは作成できません。",
              ERROR_NOT_SUPPORTED);
        }
      }
    }
  }

  report.deployment_tools_present =
      deployment_root_ok && deployment_files_ok;
  report.winpe_addon_present = winpe_root_ok && winpe_files_ok;
  report.microsoft_tools_trusted = microsoft_tools_trusted;
  report.base_layout_ready = report.deployment_tools_present &&
                             report.winpe_addon_present &&
                             report.microsoft_tools_trusted &&
                             script_readable;
  report.bootex_layout_ready =
      report.base_layout_ready && report.bootex_supported;
  if (report.base_layout_ready) {
    bool deployment_version_ok = false;
    const auto deployment_version = environment.query_msi_product_version(
        kRequiredDeploymentToolsProductCode);
    if (!deployment_version) {
      add_diagnostic(
          report,
          DiagnosticSeverity::error,
          "ADK_VERSION_QUERY_FAILED",
          report.root,
          L"Windows InstallerからADK Deployment Toolsのバージョンを読み取れませんでした。",
          deployment_version.error().native_code);
    } else {
      report.deployment_tools_version = deployment_version.value();
      deployment_version_ok =
          report.deployment_tools_version ==
          kRequiredDeploymentToolsVersion;
      if (!deployment_version_ok) {
        add_diagnostic(
            report,
            DiagnosticSeverity::error,
            "ADK_VERSION_UNSUPPORTED",
            report.root,
            L"検証済みのADK Deployment Tools 10.1.26100.2454と一致しません。",
            ERROR_REVISION_MISMATCH);
      }
    }

    bool dism_version_ok = false;
    const auto dism_version = environment.query_file_version(report.dism_path);
    if (!dism_version) {
      add_diagnostic(
          report,
          DiagnosticSeverity::error,
          "ADK_DISM_VERSION_QUERY_FAILED",
          report.dism_path,
          L"Microsoft署名済みDISMのファイルバージョンを読み取れませんでした。",
          dism_version.error().native_code);
    } else {
      report.dism_file_version = dism_version.value();
      dism_version_ok =
          report.dism_file_version == kRequiredServicedDismVersion;
      if (!dism_version_ok) {
        add_diagnostic(
            report,
            DiagnosticSeverity::error,
            "ADK_DISM_VERSION_UNSUPPORTED",
            report.dism_path,
            L"KB5101684適用後の検証済みDISM 10.0.26100.8972と一致しません。",
            ERROR_REVISION_MISMATCH);
      }
    }

    const bool oscdimg_patch_ok = verify_servicing_patch(
        report,
        environment,
        kRequiredOscdimgProductCode,
        kRequiredOscdimgPatchCode,
        report.oscdimg_servicing_patch_applied,
        "ADK_OSCDIMG_SERVICING_PATCH_MISSING",
        L"KB5101684のOscdimg更新が適用済みではありません。");
    const bool dism_patch_ok = verify_servicing_patch(
        report,
        environment,
        kRequiredDismProductCode,
        kRequiredDismPatchCode,
        report.dism_servicing_patch_applied,
        "ADK_DISM_SERVICING_PATCH_MISSING",
        L"KB5101684のDISM更新が適用済みではありません。");

    report.version_and_servicing_verified =
        deployment_version_ok && dism_version_ok && oscdimg_patch_ok &&
        dism_patch_ok;
  }
  report.media_creation_permitted =
      report.base_layout_ready && report.version_and_servicing_verified;

  if (report.base_layout_ready) {
    add_diagnostic(
        report,
        DiagnosticSeverity::information,
        "ADK_LAYOUT_READY",
        report.root,
        L"ローカルADK/WinPE Add-onの読取り専用構成検査に合格しました。");
    if (report.version_and_servicing_verified) {
      add_diagnostic(
          report,
          DiagnosticSeverity::information,
          "ADK_VERSION_SERVICING_VERIFIED",
          report.root,
          L"ADK 10.1.26100.2454とKB5101684の必須更新を読取り専用で確認しました。");
    } else {
      add_diagnostic(
          report,
          DiagnosticSeverity::warning,
          "ADK_VERSION_SERVICING_REVIEW_REQUIRED",
          report.root,
          L"検証済みADKバージョンとMicrosoftセキュリティ更新を確認できないため、メディア作成を許可しません。",
          ERROR_REVISION_MISMATCH);
    }
  }

  return report;
}

AdkDiscoveryReport detect_windows_adk(
    IAdkEnvironment& environment,
    std::wstring_view architecture) {
  AdkDiscoveryReport report{};
  report.architecture = std::wstring(architecture);

  for (const auto& root : windows_adk_candidate_roots()) {
    report.candidates.push_back(
        inspect_adk_candidate(root, architecture, environment));
    if (!report.selected_candidate_index &&
        report.candidates.back().base_layout_ready) {
      report.selected_candidate_index = report.candidates.size() - 1U;
    }
  }

  return report;
}

std::wstring_view diagnostic_severity_name(
    DiagnosticSeverity severity) noexcept {
  switch (severity) {
    case DiagnosticSeverity::information:
      return L"情報";
    case DiagnosticSeverity::warning:
      return L"警告";
    case DiagnosticSeverity::error:
      return L"エラー";
  }
  return L"不明";
}

std::wstring format_file_version(const FileVersion& version) {
  std::wostringstream output;
  output << version.major << L'.' << version.minor << L'.' << version.build
         << L'.' << version.revision;
  return output.str();
}

}  // namespace ytec::mediabuilder

#include "ytec/mediabuilder/adk_detection.h"

#include <Windows.h>

#include <filesystem>
#include <cwctype>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace {

using ytec::mediabuilder::PathKind;

struct MockEntry final {
  PathKind kind{PathKind::missing};
  std::string contents;
  bool microsoft_trusted{};
};

class MockAdkEnvironment final
    : public ytec::mediabuilder::IAdkEnvironment {
 public:
  void add_directory(const std::filesystem::path& path) {
    entries_[key(path)] = MockEntry{PathKind::directory, {}, false};
  }

  void add_file(
      const std::filesystem::path& path,
      std::string contents = {},
      bool microsoft_trusted = false) {
    entries_[key(path)] = MockEntry{
        PathKind::regular_file,
        std::move(contents),
        microsoft_trusted,
    };
  }

  void add_reparse_point(const std::filesystem::path& path) {
    entries_[key(path)] = MockEntry{PathKind::reparse_point, {}, false};
  }

  [[nodiscard]] ytec::clonecore::Result<PathKind> classify_path(
      const std::filesystem::path& path) override {
    const auto found = entries_.find(key(path));
    return ytec::clonecore::Result<PathKind>::success(
        found == entries_.end() ? PathKind::missing : found->second.kind);
  }

  [[nodiscard]] ytec::clonecore::Result<std::string> read_text_file(
      const std::filesystem::path& path,
      std::size_t maximum_bytes) override {
    const auto found = entries_.find(key(path));
    if (found == entries_.end() ||
        found->second.kind != PathKind::regular_file) {
      return ytec::clonecore::Result<std::string>::failure(
          test_error(ERROR_FILE_NOT_FOUND));
    }
    if (found->second.contents.size() > maximum_bytes) {
      return ytec::clonecore::Result<std::string>::failure(
          test_error(ERROR_FILE_TOO_LARGE));
    }
    return ytec::clonecore::Result<std::string>::success(
        found->second.contents);
  }

  [[nodiscard]] ytec::clonecore::Status verify_microsoft_signed_executable(
      const std::filesystem::path& path) override {
    const auto found = entries_.find(key(path));
    if (found != entries_.end() && found->second.microsoft_trusted) {
      return ytec::clonecore::success_status();
    }
    return ytec::clonecore::Status::failure(
        test_error(static_cast<DWORD>(TRUST_E_NOSIGNATURE)));
  }

  [[nodiscard]] ytec::clonecore::Result<ytec::mediabuilder::FileVersion>
  query_file_version(const std::filesystem::path&) override {
    return ytec::clonecore::Result<ytec::mediabuilder::FileVersion>::success(
        file_version_);
  }

  [[nodiscard]] ytec::clonecore::Result<std::wstring>
  query_msi_product_version(std::wstring_view) override {
    return ytec::clonecore::Result<std::wstring>::success(product_version_);
  }

  [[nodiscard]] ytec::clonecore::Result<bool> is_msi_patch_applied(
      std::wstring_view,
      std::wstring_view) override {
    return ytec::clonecore::Result<bool>::success(patches_applied_);
  }

  void set_file_version(ytec::mediabuilder::FileVersion version) {
    file_version_ = version;
  }

  void set_product_version(std::wstring version) {
    product_version_ = std::move(version);
  }

  void set_patches_applied(bool applied) {
    patches_applied_ = applied;
  }

 private:
  static std::wstring key(const std::filesystem::path& path) {
    std::wstring value = path.lexically_normal().native();
    for (wchar_t& character : value) {
      character = static_cast<wchar_t>(towlower(character));
    }
    return value;
  }

  static ytec::clonecore::Error test_error(DWORD native_code) {
    return ytec::clonecore::Error{
        ytec::clonecore::ErrorCode::verification_failed,
        native_code,
        L"MockAdkEnvironment",
        L"mock failure",
    };
  }

  std::map<std::wstring, MockEntry> entries_;
  ytec::mediabuilder::FileVersion file_version_{
      10U, 0U, 26100U, 8972U};
  std::wstring product_version_{L"10.1.26100.2454"};
  bool patches_applied_{true};
};

struct AdkPaths final {
  std::filesystem::path root{L"C:\\MockADK"};
  std::filesystem::path deployment{root / L"Deployment Tools"};
  std::filesystem::path winpe{root / L"Windows Preinstallation Environment"};
  std::filesystem::path copype{winpe / L"copype.cmd"};
  std::filesystem::path make_media{winpe / L"MakeWinPEMedia.cmd"};
  std::filesystem::path dism{
      deployment / L"amd64" / L"DISM" / L"dism.exe"};
  std::filesystem::path oscdimg{
      deployment / L"amd64" / L"Oscdimg" / L"oscdimg.exe"};
  std::filesystem::path wim{
      winpe / L"amd64" / L"en-us" / L"winpe.wim"};
};

void add_complete_layout(
    MockAdkEnvironment& environment,
    const AdkPaths& paths,
    std::string make_media_contents = "if /I %1==/bootex echo supported") {
  environment.add_directory(paths.root);
  environment.add_directory(paths.deployment);
  environment.add_directory(paths.winpe);
  environment.add_file(paths.copype, "@echo off");
  environment.add_file(
      paths.make_media, std::move(make_media_contents));
  environment.add_file(paths.dism, {}, true);
  environment.add_file(paths.oscdimg, {}, true);
  environment.add_file(paths.wim);
}

bool has_diagnostic(
    const ytec::mediabuilder::AdkCandidateReport& report,
    std::string_view code) {
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  return true;
}

bool test_verified_layout_permits_media_creation() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(report.deployment_tools_present, "deployment tools present") &&
         expect(report.winpe_addon_present, "WinPE add-on present") &&
         expect(report.microsoft_tools_trusted, "Microsoft tools trusted") &&
         expect(report.bootex_supported, "/bootex detected") &&
         expect(report.base_layout_ready, "base layout ready") &&
         expect(report.bootex_layout_ready, "bootex layout ready") &&
         expect(report.version_and_servicing_verified, "servicing verified") &&
         expect(report.oscdimg_servicing_patch_applied, "Oscdimg patch") &&
         expect(report.dism_servicing_patch_applied, "DISM patch") &&
         expect(report.media_creation_permitted, "media creation permitted") &&
         expect(has_diagnostic(report, "ADK_LAYOUT_READY"), "ready diagnostic") &&
         expect(
             has_diagnostic(
                 report, "ADK_VERSION_SERVICING_VERIFIED"),
             "servicing verified diagnostic");
}

bool test_missing_servicing_patch_fails_closed() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);
  environment.set_patches_applied(false);

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(report.base_layout_ready, "layout remains ready") &&
         expect(!report.version_and_servicing_verified, "servicing blocked") &&
         expect(!report.media_creation_permitted, "creation blocked") &&
         expect(
             has_diagnostic(
                 report, "ADK_OSCDIMG_SERVICING_PATCH_MISSING"),
             "Oscdimg patch diagnostic") &&
         expect(
             has_diagnostic(report, "ADK_DISM_SERVICING_PATCH_MISSING"),
             "DISM patch diagnostic");
}

bool test_unexpected_dism_version_fails_closed() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);
  environment.set_file_version({10U, 0U, 26100U, 2454U});

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(report.base_layout_ready, "layout remains ready") &&
         expect(!report.version_and_servicing_verified, "version blocked") &&
         expect(!report.media_creation_permitted, "creation blocked") &&
         expect(
             has_diagnostic(report, "ADK_DISM_VERSION_UNSUPPORTED"),
             "DISM version diagnostic");
}

bool test_unexpected_product_version_fails_closed() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);
  environment.set_product_version(L"10.1.99999.1");

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(report.base_layout_ready, "layout remains ready") &&
         expect(!report.version_and_servicing_verified, "product blocked") &&
         expect(!report.media_creation_permitted, "creation blocked") &&
         expect(
             has_diagnostic(report, "ADK_VERSION_UNSUPPORTED"),
             "product version diagnostic");
}

bool test_old_layout_stays_base_only() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths, "@echo off");

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(report.base_layout_ready, "base layout remains ready") &&
         expect(!report.bootex_layout_ready, "bootex layout blocked") &&
         expect(
             has_diagnostic(report, "ADK_BOOTEX_NOT_SUPPORTED"),
             "missing bootex warning");
}

bool test_missing_winpe_addon_fails_closed() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  environment.add_directory(paths.root);
  environment.add_directory(paths.deployment);
  environment.add_file(paths.dism, {}, true);
  environment.add_file(paths.oscdimg, {}, true);

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(!report.base_layout_ready, "base layout blocked") &&
         expect(
             has_diagnostic(report, "ADK_WINPE_ADDON_MISSING"),
             "WinPE add-on diagnostic");
}

bool test_reparse_tool_is_rejected() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);
  environment.add_reparse_point(paths.oscdimg);

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(!report.base_layout_ready, "reparse blocks readiness") &&
         expect(
             has_diagnostic(report, "ADK_REPARSE_POINT_REJECTED"),
             "reparse diagnostic");
}

bool test_unsigned_tool_is_rejected() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);
  environment.add_file(paths.oscdimg, {}, false);

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"amd64", environment);
  return expect(!report.microsoft_tools_trusted, "unsigned tool untrusted") &&
         expect(!report.base_layout_ready, "unsigned tool blocks readiness") &&
         expect(
             has_diagnostic(report, "ADK_MICROSOFT_SIGNATURE_REQUIRED"),
             "signature diagnostic");
}

bool test_non_amd64_is_rejected() {
  MockAdkEnvironment environment;
  const AdkPaths paths;
  add_complete_layout(environment, paths);

  const auto report = ytec::mediabuilder::inspect_adk_candidate(
      paths.root, L"x86", environment);
  return expect(!report.base_layout_ready, "x86 blocked") &&
         expect(
             has_diagnostic(report, "ADK_ARCHITECTURE_UNSUPPORTED"),
             "architecture diagnostic");
}

}  // namespace

int main() {
  const bool passed = test_verified_layout_permits_media_creation() &&
                      test_missing_servicing_patch_fails_closed() &&
                      test_unexpected_dism_version_fails_closed() &&
                      test_unexpected_product_version_fails_closed() &&
                      test_old_layout_stays_base_only() &&
                      test_missing_winpe_addon_fails_closed() &&
                      test_reparse_tool_is_rejected() &&
                      test_unsigned_tool_is_rejected() &&
                      test_non_amd64_is_rejected();
  if (!passed) {
    return 1;
  }
  std::cout << "PASS: MediaBuilder ADK detection tests\n";
  return 0;
}

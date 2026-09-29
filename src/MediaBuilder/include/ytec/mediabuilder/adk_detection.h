#pragma once

#include "ytec/clonecore/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::mediabuilder {

enum class PathKind : std::uint8_t {
  missing,
  directory,
  regular_file,
  reparse_point,
  other,
};

enum class DiagnosticSeverity : std::uint8_t {
  information,
  warning,
  error,
};

struct FileVersion final {
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint16_t build{};
  std::uint16_t revision{};

  [[nodiscard]] bool operator==(const FileVersion&) const noexcept = default;
};

struct AdkDiagnostic final {
  DiagnosticSeverity severity{DiagnosticSeverity::error};
  std::string code;
  std::filesystem::path path;
  std::wstring message;
  std::uint32_t native_code{};
};

struct AdkCandidateReport final {
  std::filesystem::path root;
  std::wstring architecture;
  std::filesystem::path deployment_tools_root;
  std::filesystem::path winpe_root;
  std::filesystem::path copype_path;
  std::filesystem::path make_winpe_media_path;
  std::filesystem::path dism_path;
  std::filesystem::path oscdimg_path;
  std::filesystem::path base_winpe_wim_path;
  std::wstring deployment_tools_version;
  FileVersion dism_file_version;
  bool deployment_tools_present{};
  bool winpe_addon_present{};
  bool microsoft_tools_trusted{};
  bool bootex_supported{};
  bool base_layout_ready{};
  bool bootex_layout_ready{};
  bool oscdimg_servicing_patch_applied{};
  bool dism_servicing_patch_applied{};
  bool version_and_servicing_verified{};
  bool media_creation_permitted{};
  std::vector<AdkDiagnostic> diagnostics;
};

struct AdkDiscoveryReport final {
  std::wstring architecture;
  std::vector<AdkCandidateReport> candidates;
  std::optional<std::size_t> selected_candidate_index;

  [[nodiscard]] bool base_layout_ready() const noexcept;
  [[nodiscard]] bool bootex_layout_ready() const noexcept;
  [[nodiscard]] bool media_creation_permitted() const noexcept;
};

class IAdkEnvironment {
 public:
  virtual ~IAdkEnvironment() = default;

  [[nodiscard]] virtual clonecore::Result<PathKind> classify_path(
      const std::filesystem::path& path) = 0;

  [[nodiscard]] virtual clonecore::Result<std::string> read_text_file(
      const std::filesystem::path& path,
      std::size_t maximum_bytes) = 0;

  [[nodiscard]] virtual clonecore::Status verify_microsoft_signed_executable(
      const std::filesystem::path& path) = 0;

  [[nodiscard]] virtual clonecore::Result<FileVersion> query_file_version(
      const std::filesystem::path& path) = 0;

  [[nodiscard]] virtual clonecore::Result<std::wstring>
  query_msi_product_version(std::wstring_view product_code) = 0;

  [[nodiscard]] virtual clonecore::Result<bool> is_msi_patch_applied(
      std::wstring_view product_code,
      std::wstring_view patch_code) = 0;
};

[[nodiscard]] AdkCandidateReport inspect_adk_candidate(
    const std::filesystem::path& root,
    std::wstring_view architecture,
    IAdkEnvironment& environment);

[[nodiscard]] std::vector<std::filesystem::path>
windows_adk_candidate_roots();

[[nodiscard]] AdkDiscoveryReport detect_windows_adk(
    IAdkEnvironment& environment,
    std::wstring_view architecture = L"amd64");

[[nodiscard]] std::unique_ptr<IAdkEnvironment>
make_windows_adk_environment();

[[nodiscard]] std::wstring_view diagnostic_severity_name(
    DiagnosticSeverity severity) noexcept;

[[nodiscard]] std::wstring format_file_version(
    const FileVersion& version);

}  // namespace ytec::mediabuilder

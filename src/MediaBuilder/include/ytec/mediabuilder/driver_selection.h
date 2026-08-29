#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/mediabuilder/adk_detection.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::mediabuilder {

enum class DriverOrigin : std::uint8_t {
  current_pc,
  manufacturer_folder,
};

enum class DriverCategory : std::uint8_t {
  storage_controller,
  usb_controller,
  unsupported,
};

enum class DriverArchitectureState : std::uint8_t {
  amd64_verified,
  incompatible,
  unknown,
};

enum class DriverSignatureState : std::uint8_t {
  trusted_signed,
  unsigned_or_untrusted,
  unknown,
};

enum class DriverPathState : std::uint8_t {
  verified_regular_tree,
  unsafe,
  unknown,
};

struct DriverDiagnostic final {
  DiagnosticSeverity severity{DiagnosticSeverity::error};
  std::string code;
  std::filesystem::path path;
  std::wstring message;
  std::uint32_t native_code{};
};

// This structure is the read-only evidence collected by the Windows adapter.
// The pure evaluator below deliberately does not infer missing evidence.
struct DriverCandidateEvidence final {
  std::string candidate_id;
  DriverOrigin origin{DriverOrigin::manufacturer_folder};
  DriverCategory category{DriverCategory::unsupported};
  std::wstring display_name;
  std::wstring provider;
  std::vector<std::wstring> present_device_names;
  std::filesystem::path inf_path;
  std::filesystem::path package_root;
  std::filesystem::path catalog_path;
  DriverArchitectureState architecture{DriverArchitectureState::unknown};
  DriverSignatureState signature{DriverSignatureState::unknown};
  DriverPathState path_state{DriverPathState::unknown};
  std::wstring signer;
  std::string inf_sha256;
  std::string package_tree_sha256;
  std::uint64_t package_total_bytes{};
  std::size_t package_file_count{};
  std::vector<DriverDiagnostic> diagnostics;
};

struct DriverPackageCandidate final {
  DriverCandidateEvidence evidence;
  bool selectable{};
  bool recommended_for_current_pc{};
  std::wstring decision_summary;
};

struct DriverDiscoveryReport final {
  DriverOrigin origin{DriverOrigin::current_pc};
  std::filesystem::path inspected_root;
  bool completed{};
  std::vector<DriverPackageCandidate> candidates;
  std::vector<DriverDiagnostic> diagnostics;
};

struct DriverCandidateListItemView final {
  std::string candidate_id;
  std::wstring title;
  std::wstring source_label;
  std::wstring category_label;
  std::wstring architecture_label;
  std::wstring signature_label;
  std::wstring selection_label;
  std::wstring detail;
  bool selectable{};
  bool initially_checked{};
};

struct DriverInjectionItem final {
  std::string candidate_id;
  DriverOrigin origin{DriverOrigin::current_pc};
  DriverCategory category{DriverCategory::unsupported};
  std::filesystem::path inf_path;
  std::filesystem::path package_root;
  std::filesystem::path catalog_path;
  std::string inf_sha256;
  std::string package_tree_sha256;
  std::uint64_t package_total_bytes{};
  std::size_t package_file_count{};
  std::wstring signer;
};

struct DriverInjectionCommand final {
  std::filesystem::path executable;
  std::vector<std::wstring> arguments;
};

// No mutating accessor is exposed.  The plan contains only candidates that
// appeared in the caller's explicit selection list and copies all evidence
// needed for a just-in-time fail-closed revalidation before a future launch.
class DriverInjectionPlan final {
 public:
  [[nodiscard]] const std::filesystem::path& mounted_image_root() const noexcept;
  [[nodiscard]] const std::filesystem::path& dism_path() const noexcept;
  [[nodiscard]] const std::vector<DriverInjectionItem>& items() const noexcept;
  [[nodiscard]] const std::vector<DriverInjectionCommand>& commands()
      const noexcept;

 private:
  friend clonecore::Result<DriverInjectionPlan> build_driver_injection_plan(
      const AdkCandidateReport&,
      const std::filesystem::path&,
      std::span<const DriverPackageCandidate>,
      std::span<const std::string>);

  std::filesystem::path mounted_image_root_;
  std::filesystem::path dism_path_;
  std::vector<DriverInjectionItem> items_;
  std::vector<DriverInjectionCommand> commands_;
};

[[nodiscard]] DriverPackageCandidate evaluate_driver_candidate(
    DriverCandidateEvidence evidence);

[[nodiscard]] std::vector<DriverCandidateListItemView>
make_driver_candidate_list_view(const DriverDiscoveryReport& report);

[[nodiscard]] DriverDiscoveryReport discover_current_pc_driver_candidates();

[[nodiscard]] DriverDiscoveryReport discover_manufacturer_driver_candidates(
    const std::filesystem::path& manufacturer_inf_root);

[[nodiscard]] clonecore::Result<DriverInjectionPlan>
build_driver_injection_plan(
    const AdkCandidateReport& verified_adk,
    const std::filesystem::path& mounted_image_root,
    std::span<const DriverPackageCandidate> candidates,
    std::span<const std::string> explicitly_selected_candidate_ids);

// Re-opens every source read-only, repeats path/tree/hash/architecture/trust
// checks, and verifies the Microsoft-signed ADK DISM.  It performs no WIM
// servicing and launches no process.
[[nodiscard]] clonecore::Status revalidate_driver_injection_plan(
    const DriverInjectionPlan& plan);

// The current Windows rescue-media pipeline has no typed hand-off for this
// plan yet.  Keep this false until the product WIM mount transaction consumes
// DriverInjectionPlan and calls revalidate_driver_injection_plan immediately
// before launching each command.
inline constexpr bool kDriverInjectionProductionExecutionConnected = false;

[[nodiscard]] std::wstring_view driver_origin_label(
    DriverOrigin origin) noexcept;
[[nodiscard]] std::wstring_view driver_category_label(
    DriverCategory category) noexcept;
[[nodiscard]] std::wstring_view driver_architecture_label(
    DriverArchitectureState state) noexcept;
[[nodiscard]] std::wstring_view driver_signature_label(
    DriverSignatureState state) noexcept;

}  // namespace ytec::mediabuilder

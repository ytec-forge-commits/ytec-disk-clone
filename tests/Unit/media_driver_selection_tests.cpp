#include "ytec/mediabuilder/driver_selection.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ytec::mediabuilder::DriverArchitectureState;
using ytec::mediabuilder::DriverCandidateEvidence;
using ytec::mediabuilder::DriverCategory;
using ytec::mediabuilder::DriverOrigin;
using ytec::mediabuilder::DriverPackageCandidate;
using ytec::mediabuilder::DriverPathState;
using ytec::mediabuilder::DriverSignatureState;

bool expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  return true;
}

std::string hash_text(const char character) {
  return std::string(64U, character);
}

DriverCandidateEvidence valid_evidence(
    const char id_character,
    const DriverOrigin origin = DriverOrigin::current_pc,
    const DriverCategory category = DriverCategory::storage_controller) {
  const std::wstring package = id_character == 'a' ? L"storage" : L"usb";
  return DriverCandidateEvidence{
      .candidate_id = hash_text(id_character),
      .origin = origin,
      .category = category,
      .display_name = L"検証用ドライバー",
      .provider = L"Example Driver Vendor",
      .present_device_names = origin == DriverOrigin::current_pc
                                  ? std::vector<std::wstring>{L"検証用デバイス"}
                                  : std::vector<std::wstring>{},
      .inf_path = std::filesystem::path(L"C:\\DriverStore") / package /
                  (package + L".inf"),
      .package_root = std::filesystem::path(L"C:\\DriverStore") / package,
      .catalog_path = std::filesystem::path(L"C:\\DriverStore") / package /
                      (package + L".cat"),
      .architecture = DriverArchitectureState::amd64_verified,
      .signature = DriverSignatureState::trusted_signed,
      .path_state = DriverPathState::verified_regular_tree,
      .signer = L"Example Trusted Signer",
      .inf_sha256 = hash_text('c'),
      .package_tree_sha256 = hash_text('d'),
      .package_total_bytes = 4096U,
      .package_file_count = 3U,
  };
}

ytec::mediabuilder::AdkCandidateReport verified_adk() {
  return ytec::mediabuilder::AdkCandidateReport{
      .architecture = L"amd64",
      .dism_path = L"C:\\VerifiedADK\\Deployment Tools\\amd64\\DISM\\dism.exe",
      .microsoft_tools_trusted = true,
      .media_creation_permitted = true,
  };
}

bool test_current_pc_trusted_x64_is_recommended() {
  const auto candidate = ytec::mediabuilder::evaluate_driver_candidate(
      valid_evidence('a'));
  return expect(candidate.selectable, "verified current driver selectable") &&
         expect(
             candidate.recommended_for_current_pc,
             "verified current driver recommended");
}

bool test_unsigned_unknown_and_wrong_architecture_fail_closed() {
  auto unsigned_evidence = valid_evidence('a');
  unsigned_evidence.signature =
      DriverSignatureState::unsigned_or_untrusted;
  const auto unsigned_candidate =
      ytec::mediabuilder::evaluate_driver_candidate(
          std::move(unsigned_evidence));

  auto unknown_evidence = valid_evidence('b');
  unknown_evidence.signature = DriverSignatureState::unknown;
  const auto unknown_candidate =
      ytec::mediabuilder::evaluate_driver_candidate(
          std::move(unknown_evidence));

  auto wrong_architecture = valid_evidence('c');
  wrong_architecture.architecture = DriverArchitectureState::incompatible;
  const auto incompatible_candidate =
      ytec::mediabuilder::evaluate_driver_candidate(
          std::move(wrong_architecture));

  return expect(!unsigned_candidate.selectable, "unsigned not selectable") &&
         expect(
             !unsigned_candidate.recommended_for_current_pc,
             "unsigned not recommended") &&
         expect(!unknown_candidate.selectable, "unknown trust not selectable") &&
         expect(
             !incompatible_candidate.selectable,
             "non-amd64 not selectable");
}

bool test_manufacturer_driver_requires_explicit_selection() {
  const auto candidate = ytec::mediabuilder::evaluate_driver_candidate(
      valid_evidence(
          'a',
          DriverOrigin::manufacturer_folder,
          DriverCategory::usb_controller));
  return expect(candidate.selectable, "verified manufacturer driver selectable") &&
         expect(
             !candidate.recommended_for_current_pc,
             "manufacturer driver not prechecked");
}

bool test_unsafe_tree_and_incomplete_evidence_fail_closed() {
  auto unsafe = valid_evidence('a');
  unsafe.path_state = DriverPathState::unsafe;
  const auto unsafe_candidate =
      ytec::mediabuilder::evaluate_driver_candidate(std::move(unsafe));

  auto missing_hash = valid_evidence('b');
  missing_hash.package_tree_sha256.clear();
  const auto incomplete_candidate =
      ytec::mediabuilder::evaluate_driver_candidate(
          std::move(missing_hash));

  auto unsupported = valid_evidence('c');
  unsupported.category = DriverCategory::unsupported;
  const auto unsupported_candidate =
      ytec::mediabuilder::evaluate_driver_candidate(
          std::move(unsupported));

  return expect(!unsafe_candidate.selectable, "unsafe tree not selectable") &&
         expect(
             !incomplete_candidate.selectable,
             "missing immutable fingerprint not selectable") &&
         expect(
             !unsupported_candidate.selectable,
             "unrelated device class not selectable");
}

bool test_japanese_list_view_exposes_all_decisions() {
  ytec::mediabuilder::DriverDiscoveryReport report{
      .origin = DriverOrigin::current_pc,
      .completed = true,
  };
  report.candidates.push_back(
      ytec::mediabuilder::evaluate_driver_candidate(valid_evidence('a')));
  auto unsigned_evidence = valid_evidence('b');
  unsigned_evidence.signature = DriverSignatureState::unknown;
  report.candidates.push_back(ytec::mediabuilder::evaluate_driver_candidate(
      std::move(unsigned_evidence)));

  const auto view = ytec::mediabuilder::make_driver_candidate_list_view(report);
  return expect(view.size() == 2U, "two view rows") &&
         expect(view[0].initially_checked, "current trusted row prechecked") &&
         expect(
             view[0].selection_label.find(L"現在のPC") != std::wstring::npos,
             "recommended Japanese label") &&
         expect(!view[1].selectable, "unknown row disabled") &&
         expect(
             view[1].selection_label.find(L"安全側") != std::wstring::npos,
             "fail-closed Japanese label") &&
         expect(
             view[1].signature_label.find(L"不明") != std::wstring::npos,
             "unknown trust shown");
}

bool test_plan_contains_only_explicit_selection_and_no_force_unsigned() {
  std::array<DriverPackageCandidate, 2U> candidates{
      ytec::mediabuilder::evaluate_driver_candidate(valid_evidence('a')),
      ytec::mediabuilder::evaluate_driver_candidate(valid_evidence('b')),
  };
  const std::array<std::string, 1U> selected{hash_text('b')};
  const auto plan = ytec::mediabuilder::build_driver_injection_plan(
      verified_adk(), L"C:\\WimMount", candidates, selected);
  if (!expect(static_cast<bool>(plan), "valid immutable plan")) {
    return false;
  }
  const auto& value = plan.value();
  const bool force_unsigned = std::any_of(
      value.commands()[0].arguments.begin(),
      value.commands()[0].arguments.end(),
      [](const std::wstring& argument) {
        return argument.find(L"ForceUnsigned") != std::wstring::npos;
      });
  return expect(value.items().size() == 1U, "only selected item") &&
         expect(
             value.items()[0].candidate_id == hash_text('b'),
             "exact selected id") &&
         expect(value.commands().size() == 1U, "one DISM command") &&
         expect(
             value.commands()[0].arguments[2] == L"/Add-Driver",
             "add-driver operation") &&
         expect(!force_unsigned, "ForceUnsigned never generated") &&
         expect(
             !ytec::mediabuilder::kDriverInjectionProductionExecutionConnected,
             "product execution remains honestly disconnected");
}

bool test_plan_rejects_implicit_duplicate_and_untrusted_selection() {
  std::array<DriverPackageCandidate, 1U> candidates{
      ytec::mediabuilder::evaluate_driver_candidate(valid_evidence('a')),
  };
  const std::array<std::string, 0U> empty{};
  const auto implicit = ytec::mediabuilder::build_driver_injection_plan(
      verified_adk(), L"C:\\WimMount", candidates, empty);

  const std::array<std::string, 2U> duplicate{
      hash_text('a'), hash_text('a')};
  const auto duplicate_plan = ytec::mediabuilder::build_driver_injection_plan(
      verified_adk(), L"C:\\WimMount", candidates, duplicate);

  auto untrusted_evidence = valid_evidence('b');
  untrusted_evidence.signature = DriverSignatureState::unknown;
  const std::array<DriverPackageCandidate, 1U> untrusted{
      ytec::mediabuilder::evaluate_driver_candidate(
          std::move(untrusted_evidence)),
  };
  const std::array<std::string, 1U> selected_untrusted{hash_text('b')};
  const auto untrusted_plan = ytec::mediabuilder::build_driver_injection_plan(
      verified_adk(), L"C:\\WimMount", untrusted, selected_untrusted);

  auto bad_adk = verified_adk();
  bad_adk.microsoft_tools_trusted = false;
  const std::array<std::string, 1U> selected{hash_text('a')};
  const auto untrusted_dism = ytec::mediabuilder::build_driver_injection_plan(
      bad_adk, L"C:\\WimMount", candidates, selected);

  return expect(!implicit, "implicit selection rejected") &&
         expect(!duplicate_plan, "duplicate selection rejected") &&
         expect(!untrusted_plan, "untrusted driver rejected") &&
         expect(!untrusted_dism, "untrusted DISM rejected");
}

bool test_live_current_pc_discovery_preserves_fail_closed_invariants() {
  const auto report =
      ytec::mediabuilder::discover_current_pc_driver_candidates();
  if (!expect(report.completed, "live read-only PnP discovery completed")) {
    return false;
  }
  std::size_t selectable_count{};
  for (const auto& candidate : report.candidates) {
    if (!expect(
            candidate.evidence.origin == DriverOrigin::current_pc,
            "live candidate source is current PC") ||
        !expect(
            candidate.evidence.category == DriverCategory::storage_controller ||
                candidate.evidence.category == DriverCategory::usb_controller,
            "live candidate is storage or USB only")) {
      return false;
    }
    if (candidate.selectable &&
        (!expect(
             candidate.evidence.architecture ==
                 DriverArchitectureState::amd64_verified,
             "selectable live driver is amd64") ||
         !expect(
             candidate.evidence.signature ==
                 DriverSignatureState::trusted_signed,
             "selectable live driver is trusted") ||
         !expect(
             candidate.evidence.path_state ==
                 DriverPathState::verified_regular_tree,
             "selectable live driver has safe tree"))) {
      return false;
    }
    if (candidate.selectable) {
      ++selectable_count;
    }
  }
  std::cout << "INFO: live current-PC driver candidates="
            << report.candidates.size()
            << ", selectable=" << selectable_count
            << ", diagnostics=" << report.diagnostics.size() << '\n';
  return expect(!report.candidates.empty(), "live related candidates found") &&
         expect(selectable_count != 0U, "live trusted amd64 candidate found");
}

}  // namespace

int main() {
  const bool passed = test_current_pc_trusted_x64_is_recommended() &&
                      test_unsigned_unknown_and_wrong_architecture_fail_closed() &&
                      test_manufacturer_driver_requires_explicit_selection() &&
                      test_unsafe_tree_and_incomplete_evidence_fail_closed() &&
                      test_japanese_list_view_exposes_all_decisions() &&
                      test_plan_contains_only_explicit_selection_and_no_force_unsigned() &&
                      test_plan_rejects_implicit_duplicate_and_untrusted_selection() &&
                      test_live_current_pc_discovery_preserves_fail_closed_invariants();
  if (!passed) {
    return 1;
  }
  std::cout << "PASS: MediaBuilder driver selection and immutable plan tests\n";
  return 0;
}

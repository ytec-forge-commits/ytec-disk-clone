#include "ytec/mediabuilder/driver_selection.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <utility>

namespace ytec::mediabuilder {
namespace {

constexpr std::size_t kSha256HexCharacters = 64U;

clonecore::Error plan_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"WinPEドライバー注入計画",
      .message = std::move(message),
  };
}

bool is_hex_sha256(const std::string_view value) noexcept {
  if (value.size() != kSha256HexCharacters) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool is_simple_candidate_id(const std::string_view value) noexcept {
  if (value.size() != kSha256HexCharacters) {
    return false;
  }
  return is_hex_sha256(value);
}

bool absolute_without_parent_reference(
    const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute()) {
    return false;
  }
  for (const auto& component : path) {
    if (component == L"..") {
      return false;
    }
  }
  return true;
}

bool equal_path_case_insensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
  const std::wstring left_text = left.lexically_normal().native();
  const std::wstring right_text = right.lexically_normal().native();
  if (left_text.size() > static_cast<std::size_t>(INT_MAX) ||
      right_text.size() > static_cast<std::size_t>(INT_MAX)) {
    return false;
  }
  return CompareStringOrdinal(
             left_text.data(),
             static_cast<int>(left_text.size()),
             right_text.data(),
             static_cast<int>(right_text.size()),
             TRUE) == CSTR_EQUAL;
}

bool equal_text_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  if (left.size() > static_cast<std::size_t>(INT_MAX) ||
      right.size() > static_cast<std::size_t>(INT_MAX)) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

std::wstring join_device_names(const std::vector<std::wstring>& names) {
  std::wostringstream text;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index != 0U) {
      text << L"、";
    }
    text << names[index];
  }
  return text.str();
}

}  // namespace

const std::filesystem::path& DriverInjectionPlan::mounted_image_root()
    const noexcept {
  return mounted_image_root_;
}

const std::filesystem::path& DriverInjectionPlan::dism_path() const noexcept {
  return dism_path_;
}

const std::vector<DriverInjectionItem>& DriverInjectionPlan::items()
    const noexcept {
  return items_;
}

const std::vector<DriverInjectionCommand>& DriverInjectionPlan::commands()
    const noexcept {
  return commands_;
}

DriverPackageCandidate evaluate_driver_candidate(
    DriverCandidateEvidence evidence) {
  bool structural_evidence =
      is_simple_candidate_id(evidence.candidate_id) &&
      absolute_without_parent_reference(evidence.inf_path) &&
      absolute_without_parent_reference(evidence.package_root) &&
      absolute_without_parent_reference(evidence.catalog_path) &&
      equal_text_case_insensitive(
          evidence.inf_path.extension().native(), L".inf") &&
      is_hex_sha256(evidence.inf_sha256) &&
      is_hex_sha256(evidence.package_tree_sha256) &&
      evidence.package_file_count != 0U &&
      evidence.package_total_bytes != 0U &&
      !evidence.signer.empty();

  if (evidence.category == DriverCategory::unsupported) {
    structural_evidence = false;
  }

  const bool path_verified =
      evidence.path_state == DriverPathState::verified_regular_tree;
  const bool architecture_verified =
      evidence.architecture == DriverArchitectureState::amd64_verified;
  const bool signature_verified =
      evidence.signature == DriverSignatureState::trusted_signed;

  DriverPackageCandidate candidate{
      .evidence = std::move(evidence),
      .selectable = structural_evidence && path_verified &&
                    architecture_verified && signature_verified,
  };
  candidate.recommended_for_current_pc =
      candidate.selectable &&
      candidate.evidence.origin == DriverOrigin::current_pc &&
      !candidate.evidence.present_device_names.empty();

  if (!structural_evidence) {
    candidate.decision_summary =
        L"必要な識別子、通常ファイル、署名者、またはSHA-256証跡が揃っていません。";
  } else if (!path_verified) {
    candidate.decision_summary =
        L"INFまたはパッケージ内にリンク、差替え、範囲外パス、読取り不能があります。";
  } else if (!architecture_verified) {
    candidate.decision_summary =
        candidate.evidence.architecture == DriverArchitectureState::incompatible
            ? L"x64（amd64）向けではありません。"
            : L"x64（amd64）互換性を証明できません。";
  } else if (!signature_verified) {
    candidate.decision_summary =
        candidate.evidence.signature ==
                DriverSignatureState::unsigned_or_untrusted
            ? L"署名なし、署名不正、または信頼されない署名です。"
            : L"署名と信頼チェーンを確認できません。";
  } else if (candidate.recommended_for_current_pc) {
    candidate.decision_summary =
        L"現在のx64 PCで使用中の関連デバイスに対応し、選択候補にできます。";
  } else {
    candidate.decision_summary =
        L"署名済みx64関連ドライバーです。他PC用として明示選択できます。";
  }
  return candidate;
}

std::vector<DriverCandidateListItemView> make_driver_candidate_list_view(
    const DriverDiscoveryReport& report) {
  std::vector<DriverCandidateListItemView> views;
  views.reserve(report.candidates.size());
  for (const auto& candidate : report.candidates) {
    const auto& evidence = candidate.evidence;
    std::wstring detail = candidate.decision_summary;
    if (!evidence.present_device_names.empty()) {
      detail.append(L" 対象: ");
      detail.append(join_device_names(evidence.present_device_names));
    }
    if (!evidence.provider.empty()) {
      detail.append(L" / 提供元: ");
      detail.append(evidence.provider);
    }
    if (!evidence.signer.empty()) {
      detail.append(L" / 署名者: ");
      detail.append(evidence.signer);
    }

    views.push_back(DriverCandidateListItemView{
        .candidate_id = evidence.candidate_id,
        .title = evidence.display_name.empty()
                     ? evidence.inf_path.filename().native()
                     : evidence.display_name,
        .source_label = std::wstring(driver_origin_label(evidence.origin)),
        .category_label =
            std::wstring(driver_category_label(evidence.category)),
        .architecture_label =
            std::wstring(driver_architecture_label(evidence.architecture)),
        .signature_label =
            std::wstring(driver_signature_label(evidence.signature)),
        .selection_label = candidate.selectable
            ? (candidate.recommended_for_current_pc
                   ? L"選択候補（現在のPC）"
                   : L"明示選択可能（他PC用）")
            : L"選択不可（安全側に除外）",
        .detail = std::move(detail),
        .selectable = candidate.selectable,
        .initially_checked = candidate.recommended_for_current_pc,
    });
  }
  return views;
}

clonecore::Result<DriverInjectionPlan> build_driver_injection_plan(
    const AdkCandidateReport& verified_adk,
    const std::filesystem::path& mounted_image_root,
    const std::span<const DriverPackageCandidate> candidates,
    const std::span<const std::string> explicitly_selected_candidate_ids) {
  if (!verified_adk.media_creation_permitted ||
      !verified_adk.microsoft_tools_trusted ||
      verified_adk.architecture != L"amd64" ||
      !absolute_without_parent_reference(verified_adk.dism_path) ||
      verified_adk.dism_path.filename() != L"dism.exe") {
    return clonecore::Result<DriverInjectionPlan>::failure(plan_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"署名・版・必須更新を検証済みのamd64 ADK DISMが必要です"));
  }
  if (!absolute_without_parent_reference(mounted_image_root)) {
    return clonecore::Result<DriverInjectionPlan>::failure(plan_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"WIMマウント先は絶対パスで指定する必要があります"));
  }
  if (explicitly_selected_candidate_ids.empty()) {
    return clonecore::Result<DriverInjectionPlan>::failure(plan_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"注入するドライバーを明示選択してください"));
  }

  std::set<std::string> unique_ids;
  DriverInjectionPlan plan;
  plan.mounted_image_root_ = mounted_image_root.lexically_normal();
  plan.dism_path_ = verified_adk.dism_path.lexically_normal();

  for (const auto& selected_id : explicitly_selected_candidate_ids) {
    if (!is_simple_candidate_id(selected_id) ||
        !unique_ids.insert(selected_id).second) {
      return clonecore::Result<DriverInjectionPlan>::failure(plan_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"選択識別子が不正または重複しています"));
    }
    const auto found = std::find_if(
        candidates.begin(), candidates.end(), [&selected_id](const auto& item) {
          return item.evidence.candidate_id == selected_id;
        });
    if (found == candidates.end() || !found->selectable ||
        found->evidence.architecture !=
            DriverArchitectureState::amd64_verified ||
        found->evidence.signature != DriverSignatureState::trusted_signed ||
        found->evidence.path_state !=
            DriverPathState::verified_regular_tree) {
      return clonecore::Result<DriverInjectionPlan>::failure(plan_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"未確認、未署名、x64不一致、または選択不可のドライバーが含まれています"));
    }

    const auto& evidence = found->evidence;
    plan.items_.push_back(DriverInjectionItem{
        .candidate_id = evidence.candidate_id,
        .origin = evidence.origin,
        .category = evidence.category,
        .inf_path = evidence.inf_path,
        .package_root = evidence.package_root,
        .catalog_path = evidence.catalog_path,
        .inf_sha256 = evidence.inf_sha256,
        .package_tree_sha256 = evidence.package_tree_sha256,
        .package_total_bytes = evidence.package_total_bytes,
        .package_file_count = evidence.package_file_count,
        .signer = evidence.signer,
    });
    plan.commands_.push_back(DriverInjectionCommand{
        .executable = plan.dism_path_,
        .arguments = {
            L"/English",
            L"/Image:" + plan.mounted_image_root_.native(),
            L"/Add-Driver",
            L"/Driver:" + evidence.inf_path.native(),
        },
    });
  }

  return clonecore::Result<DriverInjectionPlan>::success(std::move(plan));
}

std::wstring_view driver_origin_label(const DriverOrigin origin) noexcept {
  switch (origin) {
    case DriverOrigin::current_pc:
      return L"現在のPC";
    case DriverOrigin::manufacturer_folder:
      return L"他PC用メーカーINFフォルダー";
  }
  return L"不明";
}

std::wstring_view driver_category_label(
    const DriverCategory category) noexcept {
  switch (category) {
    case DriverCategory::storage_controller:
      return L"ストレージ";
    case DriverCategory::usb_controller:
      return L"USB";
    case DriverCategory::unsupported:
      return L"対象外";
  }
  return L"対象外";
}

std::wstring_view driver_architecture_label(
    const DriverArchitectureState state) noexcept {
  switch (state) {
    case DriverArchitectureState::amd64_verified:
      return L"x64確認済み";
    case DriverArchitectureState::incompatible:
      return L"x64非対応";
    case DriverArchitectureState::unknown:
      return L"x64互換性不明";
  }
  return L"x64互換性不明";
}

std::wstring_view driver_signature_label(
    const DriverSignatureState state) noexcept {
  switch (state) {
    case DriverSignatureState::trusted_signed:
      return L"署名・信頼確認済み";
    case DriverSignatureState::unsigned_or_untrusted:
      return L"未署名または信頼不可";
    case DriverSignatureState::unknown:
      return L"署名状態不明";
  }
  return L"署名状態不明";
}

}  // namespace ytec::mediabuilder

#include "ytec/bootrepair/mbr2gpt.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::bootrepair {
namespace {

clonecore::Error conversion_error(
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

bool is_drive_absolute_path(const std::wstring& path) {
  return path.size() >= 3 && std::iswalpha(path[0]) != 0 && path[1] == L':' &&
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
         normalized.ends_with(L"\\..") || normalized.starts_with(L"..\\");
}

std::wstring normalize_slashes(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3 && value.back() == L'\\') {
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
  return _wcsnicmp(ending.data(), suffix.data(), suffix.size()) == 0;
}

clonecore::Status validate_system_directory(const std::wstring& directory) {
  if (!is_drive_absolute_path(directory) || has_forbidden_path_text(directory) ||
      !ends_with_case_insensitive(
          normalize_slashes(directory), L"\\Windows\\System32")) {
    return clonecore::Status::failure(conversion_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"MBR2GPTシステムディレクトリ",
        L"現在のWinPEのSystem32絶対パスが必要です"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_observed_target(
    const Mbr2GptConversionRequest& request,
    const clonecore::StableDiskIdentity& observed) {
  if (observed.disk_number != request.candidate_disk_number) {
    return clonecore::Status::failure(conversion_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"MBR2GPT変換先ディスク番号",
        L"再列挙した変換先が指定ディスク番号に存在しません"));
  }
  const auto identity = clonecore::validate_stable_identity(
      request.expected_target, observed, L"MBR2GPT変換先");
  if (!identity) {
    return identity;
  }
  if (request.expected_target.is_system_disk || observed.is_system_disk) {
    return clonecore::Status::failure(conversion_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"MBR2GPT実行中システム保護",
        L"実行中のWindowsまたはWinPEのシステムディスクは変換先にできません"));
  }
  if (!request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token !=
          clonecore::make_target_confirmation_token(observed)) {
    return clonecore::Status::failure(conversion_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"MBR2GPT二段階確認",
        L"変換確認または変換先固有の確認文字列が一致しません"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::vector<std::wstring>> build_mbr2gpt_arguments(
    const Mbr2GptAction action,
    const std::uint32_t disk_number) {
  if (action != Mbr2GptAction::validate && action != Mbr2GptAction::convert) {
    return clonecore::Result<std::vector<std::wstring>>::failure(
        conversion_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"MBR2GPT動作種別",
            L"validateまたはconvertだけを指定できます"));
  }
  return clonecore::Result<std::vector<std::wstring>>::success({
      action == Mbr2GptAction::validate ? L"/validate" : L"/convert",
      L"/disk:" + std::to_wstring(disk_number),
  });
}

clonecore::Result<Mbr2GptConversionReport> execute_mbr2gpt_conversion(
    const Mbr2GptConversionRequest& request,
    const std::wstring& trusted_system_directory,
    IMbr2GptTargetObserver& target_observer,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner) {
  const auto system_status =
      validate_system_directory(trusted_system_directory);
  if (!system_status) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        system_status.error());
  }

  const auto observed_before_validation =
      target_observer.observe_target(request.candidate_disk_number);
  if (!observed_before_validation) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        observed_before_validation.error());
  }
  const auto initial_target_status =
      validate_observed_target(request, observed_before_validation.value());
  if (!initial_target_status) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        initial_target_status.error());
  }

  const std::wstring system_directory =
      normalize_slashes(trusted_system_directory);
  const std::wstring executable_path =
      system_directory + L"\\mbr2gpt.exe";
  const auto trust_status =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!trust_status) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        trust_status.error());
  }

  const auto validation_arguments = build_mbr2gpt_arguments(
      Mbr2GptAction::validate, request.candidate_disk_number);
  if (!validation_arguments) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        validation_arguments.error());
  }
  const auto validation = process_runner.run(
      executable_path,
      validation_arguments.value(),
      system_directory);
  if (!validation) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        validation.error());
  }
  if (validation.value().exit_code != 0) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        conversion_error(
            clonecore::ErrorCode::verification_failed,
            validation.value().exit_code,
            L"MBR2GPT validate終了コード",
            L"Microsoft MBR2GPTの事前検証が失敗したため変換を開始しません"));
  }

  const auto observed_before_conversion =
      target_observer.observe_target(request.candidate_disk_number);
  if (!observed_before_conversion) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        observed_before_conversion.error());
  }
  const auto final_target_status =
      validate_observed_target(request, observed_before_conversion.value());
  if (!final_target_status) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        final_target_status.error());
  }

  const auto conversion_arguments = build_mbr2gpt_arguments(
      Mbr2GptAction::convert, request.candidate_disk_number);
  if (!conversion_arguments) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        conversion_arguments.error());
  }
  const auto conversion = process_runner.run(
      executable_path,
      conversion_arguments.value(),
      system_directory);
  if (!conversion) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        conversion.error());
  }
  if (conversion.value().exit_code != 0) {
    return clonecore::Result<Mbr2GptConversionReport>::failure(
        conversion_error(
            clonecore::ErrorCode::verification_failed,
            conversion.value().exit_code,
            L"MBR2GPT convert終了コード",
            L"Microsoft MBR2GPTの変換が失敗したため成功扱いにできません"));
  }

  return clonecore::Result<Mbr2GptConversionReport>::success(
      Mbr2GptConversionReport{
          .executable_path = executable_path,
          .disk_number = request.candidate_disk_number,
          .validation = validation.value(),
          .conversion = conversion.value(),
          .microsoft_signature_verified = true,
          .target_reidentified_before_conversion = true,
      });
}

}  // namespace ytec::bootrepair

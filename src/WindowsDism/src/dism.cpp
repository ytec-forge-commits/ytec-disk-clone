#include "ytec/windowsdism/dism.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <utility>

namespace ytec::windowsdism {
namespace {

clonecore::Error dism_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(dism_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool contains_control_character(const std::wstring_view value) noexcept {
  return std::any_of(value.begin(), value.end(), [](const wchar_t character) {
    return character == L'\0' || character == L'\r' || character == L'\n';
  });
}

bool is_absolute_windows_path(const std::wstring_view value) noexcept {
  if (value.size() >= 4U &&
      (value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\"))) {
    return true;
  }
  return value.size() >= 3U &&
      ((value[0] >= L'A' && value[0] <= L'Z') ||
       (value[0] >= L'a' && value[0] <= L'z')) &&
      value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

bool has_wim_extension(const std::wstring_view value) noexcept {
  constexpr std::wstring_view extension = L".wim";
  if (value.size() <= extension.size()) {
    return false;
  }
  const auto suffix = value.substr(value.size() - extension.size());
  return std::equal(
      suffix.begin(), suffix.end(), extension.begin(), [](wchar_t left, wchar_t right) {
        return std::towlower(left) == std::towlower(right);
      });
}

clonecore::Status validate_absolute_path(
    const std::wstring_view value,
    const std::wstring_view field_name) {
  if (value.empty() || contains_control_character(value) ||
      !is_absolute_windows_path(value)) {
    return clonecore::Status::failure(dism_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"DISM引数検証",
        std::wstring(field_name) + L"は絶対パスで指定してください"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_image_path(const std::wstring_view value) {
  const auto path = validate_absolute_path(value, L"WIMファイル");
  if (!path) {
    return path;
  }
  if (!has_wim_extension(value)) {
    return clonecore::Status::failure(dism_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"DISM引数検証",
        L"WIMファイルの拡張子が.wimではありません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::wstring> dism_executable_path(
    const std::wstring& trusted_system_directory) {
  const auto directory =
      validate_absolute_path(trusted_system_directory, L"信頼済みSystem32");
  if (!directory) {
    return clonecore::Result<std::wstring>::failure(directory.error());
  }
  std::wstring path = trusted_system_directory;
  if (!path.ends_with(L'\\') && !path.ends_with(L'/')) {
    path.push_back(L'\\');
  }
  path.append(L"dism.exe");
  return clonecore::Result<std::wstring>::success(std::move(path));
}

clonecore::Result<DismExecutionReport> execute_dism(
    const std::vector<std::wstring>& arguments,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const bootrepair::ProcessOutputCallback& output_callback,
    const std::wstring_view operation) {
  auto executable = dism_executable_path(trusted_system_directory);
  if (!executable) {
    return clonecore::Result<DismExecutionReport>::failure(executable.error());
  }
  const auto initial_trust =
      trust_verifier.verify_microsoft_signed(executable.value());
  if (!initial_trust) {
    return clonecore::Result<DismExecutionReport>::failure(
        initial_trust.error());
  }
  auto process = process_runner.run_streamed(
      executable.value(), arguments, trusted_system_directory, output_callback);
  if (!process) {
    return clonecore::Result<DismExecutionReport>::failure(process.error());
  }
  const auto final_trust =
      trust_verifier.verify_microsoft_signed(executable.value());
  if (!final_trust) {
    return clonecore::Result<DismExecutionReport>::failure(final_trust.error());
  }
  if (process.value().exit_code != 0U) {
    return failure<DismExecutionReport>(
        clonecore::ErrorCode::io_failed,
        process.value().exit_code,
        std::wstring(operation),
        L"Microsoft DISMがエラーを返しました");
  }
  return clonecore::Result<DismExecutionReport>::success(
      DismExecutionReport{
          .executable_path = executable.take_value(),
          .exit_code = process.value().exit_code,
          .standard_output = std::move(process.value().standard_output),
          .standard_error = std::move(process.value().standard_error),
          .microsoft_signature_verified = true,
      });
}

}  // namespace

clonecore::Result<std::vector<std::wstring>> build_dism_capture_arguments(
    const DismCaptureRequest& request) {
  const auto source = validate_absolute_path(request.source_root, L"読み取り元");
  const auto image = validate_image_path(request.image_path);
  const auto scratch =
      validate_absolute_path(request.scratch_directory, L"作業フォルダー");
  if (!source) {
    return clonecore::Result<std::vector<std::wstring>>::failure(source.error());
  }
  if (!image) {
    return clonecore::Result<std::vector<std::wstring>>::failure(image.error());
  }
  if (!scratch) {
    return clonecore::Result<std::vector<std::wstring>>::failure(scratch.error());
  }
  if (request.image_name.empty() || request.image_name.size() > 128U ||
      contains_control_character(request.image_name)) {
    return failure<std::vector<std::wstring>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"DISM引数検証",
        L"WIMイメージ名が不正です");
  }
  return clonecore::Result<std::vector<std::wstring>>::success({
      L"/Capture-Image",
      L"/ImageFile:" + request.image_path,
      L"/CaptureDir:" + request.source_root,
      L"/Name:" + request.image_name,
      L"/Compress:fast",
      L"/CheckIntegrity",
      L"/Verify",
      L"/EA",
      L"/ScratchDir:" + request.scratch_directory,
  });
}

clonecore::Result<std::vector<std::wstring>> build_dism_apply_arguments(
    const DismApplyRequest& request) {
  const auto image = validate_image_path(request.image_path);
  const auto target = validate_absolute_path(request.target_root, L"復元先");
  const auto scratch =
      validate_absolute_path(request.scratch_directory, L"作業フォルダー");
  if (!image) {
    return clonecore::Result<std::vector<std::wstring>>::failure(image.error());
  }
  if (!target) {
    return clonecore::Result<std::vector<std::wstring>>::failure(target.error());
  }
  if (!scratch) {
    return clonecore::Result<std::vector<std::wstring>>::failure(scratch.error());
  }
  return clonecore::Result<std::vector<std::wstring>>::success({
      L"/Apply-Image",
      L"/ImageFile:" + request.image_path,
      L"/Index:1",
      L"/ApplyDir:" + request.target_root,
      L"/CheckIntegrity",
      L"/Verify",
      L"/EA",
      L"/ScratchDir:" + request.scratch_directory,
  });
}

clonecore::Result<DismExecutionReport> execute_dism_capture(
    const DismCaptureRequest& request,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const bootrepair::ProcessOutputCallback& output_callback) {
  const auto arguments = build_dism_capture_arguments(request);
  if (!arguments) {
    return clonecore::Result<DismExecutionReport>::failure(arguments.error());
  }
  return execute_dism(
      arguments.value(),
      trusted_system_directory,
      trust_verifier,
      process_runner,
      output_callback,
      L"縮小移行WIM作成");
}

clonecore::Result<DismExecutionReport> execute_dism_apply(
    const DismApplyRequest& request,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const bootrepair::ProcessOutputCallback& output_callback) {
  const auto arguments = build_dism_apply_arguments(request);
  if (!arguments) {
    return clonecore::Result<DismExecutionReport>::failure(arguments.error());
  }
  return execute_dism(
      arguments.value(),
      trusted_system_directory,
      trust_verifier,
      process_runner,
      output_callback,
      L"縮小移行WIM復元");
}

}  // namespace ytec::windowsdism

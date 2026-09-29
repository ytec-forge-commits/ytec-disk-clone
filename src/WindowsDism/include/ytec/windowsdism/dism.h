#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::windowsdism {

struct DismCaptureRequest final {
  std::wstring source_root;
  std::wstring image_path;
  std::wstring scratch_directory;
  std::wstring image_name;
};

struct DismApplyRequest final {
  std::wstring image_path;
  std::wstring target_root;
  std::wstring scratch_directory;
};

struct DismExecutionReport final {
  std::wstring executable_path;
  std::uint32_t exit_code{};
  std::string standard_output;
  std::string standard_error;
  bool microsoft_signature_verified{};
};

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_dism_capture_arguments(const DismCaptureRequest& request);

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_dism_apply_arguments(const DismApplyRequest& request);

[[nodiscard]] clonecore::Result<DismExecutionReport> execute_dism_capture(
    const DismCaptureRequest& request,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const bootrepair::ProcessOutputCallback& output_callback = {});

[[nodiscard]] clonecore::Result<DismExecutionReport> execute_dism_apply(
    const DismApplyRequest& request,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const bootrepair::ProcessOutputCallback& output_callback = {});

}  // namespace ytec::windowsdism

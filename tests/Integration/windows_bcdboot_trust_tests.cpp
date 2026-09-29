#include "ytec/bootrepair/bcdboot.h"

#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

std::wstring system_tool_path(const wchar_t* const file_name) {
  std::vector<wchar_t> system_directory(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(
      system_directory.data(),
      static_cast<UINT>(system_directory.size()));
  if (length == 0 || length >= system_directory.size()) {
    return {};
  }
  return std::wstring(system_directory.data(), length) + L"\\" + file_name;
}

std::wstring current_executable_path() {
  std::vector<wchar_t> path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    return {};
  }
  return std::wstring(path.data(), length);
}

}  // namespace

int wmain() {
  const std::wstring bcdboot = system_tool_path(L"bcdboot.exe");
  const std::wstring mbr2gpt = system_tool_path(L"mbr2gpt.exe");
  const std::wstring unsigned_test_binary = current_executable_path();
  if (bcdboot.empty() || mbr2gpt.empty() || unsigned_test_binary.empty()) {
    std::cerr << "FAIL: Windows system paths could not be resolved\n";
    return 1;
  }

  auto verifier = ytec::bootrepair::make_windows_authenticode_verifier();
  const auto trusted = verifier->verify_microsoft_signed(bcdboot);
  if (!trusted) {
    std::wcerr << L"FAIL: system BCDBoot trust verification failed; native="
               << trusted.error().native_code << L" operation="
               << trusted.error().operation << L" message="
               << trusted.error().message << L'\n';
    return 1;
  }

  const auto conversion_tool_trusted =
      verifier->verify_microsoft_signed(mbr2gpt);
  if (!conversion_tool_trusted) {
    std::wcerr << L"FAIL: system MBR2GPT trust verification failed; native="
               << conversion_tool_trusted.error().native_code
               << L" operation=" << conversion_tool_trusted.error().operation
               << L" message=" << conversion_tool_trusted.error().message
               << L'\n';
    return 1;
  }

  const auto untrusted =
      verifier->verify_microsoft_signed(unsigned_test_binary);
  if (untrusted) {
    std::cerr << "FAIL: unsigned project test binary was trusted\n";
    return 1;
  }

  std::cout << "PASS: catalog-signed system BCDBoot and MBR2GPT trusted; "
               "unsigned project binary rejected\n";
  return 0;
}

#pragma once

#include "ytec/clonecore/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ytec::bootrepair {

enum class PeArchitecture : std::uint8_t {
  x86,
  amd64,
  arm64,
  unknown,
};

struct OfflineWindowsVersion final {
  std::uint32_t major{};
  std::uint32_t build{};
  std::wstring installation_type;
};

// Parses only the bounded DOS/COFF/optional-header prefix needed to identify
// an offline Windows kernel. The caller must treat the input as untrusted.
[[nodiscard]] clonecore::Result<PeArchitecture> inspect_pe_architecture(
    std::span<const std::byte> image_prefix);

[[nodiscard]] bool is_supported_offline_windows_version(
    const OfflineWindowsVersion& version) noexcept;

// Accepts only a drive root (for example, C:\\) or an exact Volume GUID root
// (for example, \\\\?\\Volume{GUID}\\). The returned root always ends in a
// backslash and never contains a child path. This boundary prevents an
// untrusted UI value from redirecting the offline-Windows probe elsewhere.
[[nodiscard]] clonecore::Result<std::wstring>
normalize_offline_windows_volume_root(std::wstring_view volume_root);

// Opens a standalone SOFTWARE hive without modifying or replaying it. The
// Windows Offline Registry API is preferred when present; otherwise the
// dependency-free bounded REGF reader is used. No offline-registry save
// function is ever called.
[[nodiscard]] clonecore::Result<OfflineWindowsVersion>
read_offline_windows_version_hive(const std::wstring& hive_path);

// Supported boot repair and direct-clone finalization target Windows 10/11 x64
// only. The input is a drive or Volume GUID root. This opens
// Windows\System32\ntoskrnl.exe and the offline SOFTWARE registry hive read-only,
// rejects reparse points, verifies AMD64 PE32+, and accepts only client
// Windows major version 10 with build 10240 or newer.
[[nodiscard]] clonecore::Status verify_offline_windows_amd64(
    const std::wstring& windows_root);

}  // namespace ytec::bootrepair

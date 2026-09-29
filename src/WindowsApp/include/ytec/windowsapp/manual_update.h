#pragma once

#include "ytec/clonecore/result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

inline constexpr std::wstring_view kManualUpdateManifestUrl{
    L"https://ytec.cloudfree.jp/forge/updates/tsumugi-drive/update-v1.json"};
inline constexpr std::wstring_view kManualUpdateReleasePageUrl{
    L"https://ytec.cloudfree.jp/forge/projects/tsumugi-drive/"};
inline constexpr std::size_t kMaximumManualUpdateManifestBytes{16U * 1024U};

struct ManualUpdateManifest final {
  std::uint32_t schema_version{};
  std::string product_id;
  std::string latest_version;
  std::wstring release_page_url;
  std::string published_utc;
  std::string package_sha256;
};

struct ManualUpdateTransportResponse final {
  std::uint32_t http_status{};
  std::wstring final_url;
  std::wstring content_type;
  std::vector<std::byte> body;
};

using ManualUpdateTransport = std::function<
    clonecore::Result<ManualUpdateTransportResponse>(
        std::wstring_view fixed_url,
        std::size_t maximum_response_bytes)>;

enum class ManualUpdateDisposition : std::uint8_t {
  up_to_date,
  update_available,
  local_version_newer,
};

struct ManualUpdateCheckRequest final {
  bool user_initiated{};
  std::string current_version;
};

struct ManualUpdateCheckReport final {
  ManualUpdateDisposition disposition{ManualUpdateDisposition::up_to_date};
  std::string current_version;
  ManualUpdateManifest manifest;
};

// Parses the fixed, flat update-v1 JSON schema. The response is intentionally
// bounded and ASCII-only so that an update server cannot drive arbitrary UI
// text or nested allocations in the product.
[[nodiscard]] clonecore::Result<ManualUpdateManifest>
parse_manual_update_manifest(std::span<const std::byte> body);

// Invokes the transport only after an explicit user action. This function
// never downloads or launches a package; it returns display-only metadata.
[[nodiscard]] clonecore::Result<ManualUpdateCheckReport>
check_manual_update(
    const ManualUpdateCheckRequest& request,
    const ManualUpdateTransport& transport);

// Constructs the only production transport for the manual Y-TEC update
// endpoint. Construction performs no communication; the returned callable
// performs one bounded HTTPS GET only when check_manual_update has accepted an
// explicit user action.
[[nodiscard]] ManualUpdateTransport make_windows_manual_update_transport();

[[nodiscard]] clonecore::Result<ManualUpdateCheckReport>
check_manual_update_with_windows_apis(
    const ManualUpdateCheckRequest& request);

}  // namespace ytec::windowsapp

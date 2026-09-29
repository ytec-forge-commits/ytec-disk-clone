#include "ytec/windowsapp/manual_update.h"

#include "ytec/clonecore/error.h"

#include <Windows.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ytec::windowsapp::ManualUpdateCheckRequest;
using ytec::windowsapp::ManualUpdateDisposition;
using ytec::windowsapp::ManualUpdateTransportResponse;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const char value : text) {
    result.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(value)));
  }
  return result;
}

std::string valid_manifest(std::string_view version = "1.0.0") {
  return std::string{
             "{\"schemaVersion\":1,"
             "\"productId\":\"ytec-tsumugi-drive\","
             "\"latestVersion\":\""} +
         std::string(version) +
         "\",\"releasePageUrl\":\"https://ytec.cloudfree.jp/forge/projects/"
         "tsumugi-drive/\","
         "\"publishedUtc\":\"2026-08-11T12:34:56Z\","
         "\"packageSha256\":\""
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
         "\"}";
}

ManualUpdateTransportResponse valid_response(
    std::string_view version = "1.0.0") {
  return ManualUpdateTransportResponse{
      .http_status = 200U,
      .final_url = std::wstring(
          ytec::windowsapp::kManualUpdateManifestUrl),
      .content_type = L"application/json; charset=us-ascii",
      .body = bytes(valid_manifest(version)),
  };
}

void explicit_user_action_precedes_transport() {
  std::size_t calls{};
  const auto result = ytec::windowsapp::check_manual_update(
      ManualUpdateCheckRequest{
          .user_initiated = false,
          .current_version = "1.0.0",
      },
      [&](std::wstring_view, std::size_t) {
        ++calls;
        return ytec::clonecore::Result<ManualUpdateTransportResponse>::success(
            valid_response());
      });
  check(!result.has_value(), "A non-user-initiated check must fail closed");
  check(calls == 0U, "A non-user-initiated check must perform zero transport calls");
  check(
      result.error().code ==
          ytec::clonecore::ErrorCode::confirmation_required,
      "The gate should preserve an explicit confirmation-required error");
}

void fixed_schema_is_parsed_and_normalized() {
  const auto result = ytec::windowsapp::parse_manual_update_manifest(
      bytes(valid_manifest("1.2.3-beta.2")));
  check(result.has_value(), "The fixed update-v1 schema should parse");
  check(
      result.value().schema_version == 1U &&
          result.value().product_id == "ytec-tsumugi-drive" &&
          result.value().latest_version == "1.2.3-beta.2",
      "The parsed manifest should preserve its bounded identity fields");
  check(
      result.value().package_sha256 ==
          "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF",
      "The package digest should normalize to uppercase ASCII");
}

void version_comparison_is_semver_compatible() {
  const auto run = [](std::string current, std::string latest) {
    return ytec::windowsapp::check_manual_update(
        ManualUpdateCheckRequest{
            .user_initiated = true,
            .current_version = std::move(current),
        },
        [latest = std::move(latest)](std::wstring_view url, std::size_t limit) {
          check(
              url == ytec::windowsapp::kManualUpdateManifestUrl,
              "Only the fixed Y-TEC endpoint may reach the transport");
          check(
              limit == ytec::windowsapp::kMaximumManualUpdateManifestBytes,
              "The response cap must be passed to the transport");
          return ytec::clonecore::Result<ManualUpdateTransportResponse>::success(
              valid_response(latest));
        });
  };

  const auto available = run("0.2.0-dev", "1.0.0-beta.1");
  check(
      available.has_value() &&
          available.value().disposition ==
              ManualUpdateDisposition::update_available,
      "A later prerelease should be reported as available");

  const auto current = run("1.0.0-beta.2", "1.0.0-beta.2");
  check(
      current.has_value() &&
          current.value().disposition == ManualUpdateDisposition::up_to_date,
      "Identical prereleases should be current");

  const auto newer = run("1.0.0", "1.0.0-beta.9");
  check(
      newer.has_value() &&
          newer.value().disposition ==
              ManualUpdateDisposition::local_version_newer,
      "A stable local build should be newer than its prerelease");
}

void response_envelope_is_fail_closed() {
  const auto run = [](ManualUpdateTransportResponse response) {
    return ytec::windowsapp::check_manual_update(
        ManualUpdateCheckRequest{
            .user_initiated = true,
            .current_version = "1.0.0",
        },
        [response = std::move(response)](std::wstring_view, std::size_t) mutable {
          return ytec::clonecore::Result<ManualUpdateTransportResponse>::success(
              std::move(response));
        });
  };

  auto redirected = valid_response();
  redirected.final_url = L"https://example.invalid/update.json";
  check(!run(std::move(redirected)).has_value(), "Redirect drift must fail");

  auto wrong_type = valid_response();
  wrong_type.content_type = L"text/html";
  check(!run(std::move(wrong_type)).has_value(), "Non-JSON content must fail");

  auto wrong_status = valid_response();
  wrong_status.http_status = 304U;
  check(!run(std::move(wrong_status)).has_value(), "Non-200 status must fail");

  auto oversized = valid_response();
  oversized.body.assign(
      ytec::windowsapp::kMaximumManualUpdateManifestBytes + 1U,
      std::byte{'x'});
  check(!run(std::move(oversized)).has_value(), "Oversized responses must fail");
}

void malformed_or_drifted_manifests_are_rejected() {
  const auto reject = [](std::string text) {
    return !ytec::windowsapp::parse_manual_update_manifest(bytes(text))
                .has_value();
  };

  check(reject("{}"), "Missing required fields must fail");
  check(
      reject(valid_manifest() + " trailing"),
      "Trailing content must fail");

  std::string duplicate = valid_manifest();
  const auto closing = duplicate.rfind('}');
  duplicate.insert(closing, ",\"productId\":\"ytec-tsumugi-drive\"");
  check(reject(std::move(duplicate)), "Duplicate fields must fail");

  std::string unknown = valid_manifest();
  unknown.insert(unknown.rfind('}'), ",\"notes\":\"server text\"");
  check(reject(std::move(unknown)), "Unknown fields must fail");

  std::string wrong_product = valid_manifest();
  const auto product = wrong_product.find("ytec-tsumugi-drive");
  wrong_product.replace(product, 18U, "another-product");
  check(reject(std::move(wrong_product)), "Product drift must fail");

  std::string wrong_url = valid_manifest();
  const auto host = wrong_url.find("ytec.cloudfree.jp");
  wrong_url.replace(host, 17U, "example.invalid");
  check(reject(std::move(wrong_url)), "Release page origin drift must fail");

  std::string wrong_hash = valid_manifest();
  const auto digest = wrong_hash.find("0123456789abcdef");
  wrong_hash[digest] = 'z';
  check(reject(std::move(wrong_hash)), "A non-hex digest must fail");

  check(
      reject(valid_manifest("01.0.0")),
      "Non-canonical version numbers must fail");
}

void transport_failures_are_preserved() {
  const auto result = ytec::windowsapp::check_manual_update(
      ManualUpdateCheckRequest{
          .user_initiated = true,
          .current_version = "1.0.0",
      },
      [](std::wstring_view, std::size_t) {
        return ytec::clonecore::Result<ManualUpdateTransportResponse>::failure(
            ytec::clonecore::Error{
                .code = ytec::clonecore::ErrorCode::query_failed,
                .native_code = ERROR_TIMEOUT,
                .operation = L"fake update transport",
                .message = L"timeout",
            });
      });
  check(!result.has_value(), "Transport errors must remain failures");
  check(
      result.error().native_code == ERROR_TIMEOUT &&
          result.error().operation == L"fake update transport",
      "Transport diagnostics should pass through without rewriting");
}

}  // namespace

int main() {
  try {
    explicit_user_action_precedes_transport();
    fixed_schema_is_parsed_and_normalized();
    version_comparison_is_semver_compatible();
    response_envelope_is_fail_closed();
    malformed_or_drifted_manifests_are_rejected();
    transport_failures_are_preserved();
    std::cout << "manual update tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "manual update tests: FAIL: " << exception.what() << '\n';
    return 1;
  }
}

#include "ytec/uisupport/error_presentation.h"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const char* message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void check(const bool condition, const char* message) {
  if (!condition) {
    fail(message);
  }
}

void check_not_contains(
    const std::wstring& value,
    const std::wstring& forbidden,
    const char* message) {
  check(value.find(forbidden) == std::wstring::npos, message);
}

void test_all_codes_have_bounded_primary_text() {
  using ytec::clonecore::ErrorCode;
  static constexpr std::array<ErrorCode, 13U> codes{
      ErrorCode::invalid_argument,
      ErrorCode::invalid_data,
      ErrorCode::access_denied,
      ErrorCode::enumeration_failed,
      ErrorCode::query_failed,
      ErrorCode::io_failed,
      ErrorCode::identity_mismatch,
      ErrorCode::confirmation_required,
      ErrorCode::unsupported_layout,
      ErrorCode::verification_failed,
      ErrorCode::unsupported_platform,
      ErrorCode::internal_error,
      ErrorCode::cancelled,
  };
  for (const ErrorCode code : codes) {
    const ytec::clonecore::Error error{
        .code = code,
        .native_code = ERROR_ACCESS_DENIED,
        .operation = L"raw operation must stay in details",
        .message = L"raw message must stay in details",
    };
    const auto presentation =
        ytec::uisupport::make_error_presentation(error);
    const std::wstring primary =
        ytec::uisupport::format_error_primary(presentation);
    check(!presentation.summary.empty(), "summary must not be empty");
    check(!presentation.code.empty(), "code must not be empty");
    check(!presentation.next_action.empty(), "next action must not be empty");
    check(
        presentation.details_expandable && presentation.details_copyable,
        "bounded details must be expandable and copyable");
    check(
        presentation.summary.size() <=
            ytec::uisupport::kMaximumErrorSummaryCharacters,
        "summary exceeded its public bound");
    check(
        presentation.code.size() <=
            ytec::uisupport::kMaximumErrorCodeCharacters,
        "code exceeded its public bound");
    check(
        presentation.next_action.size() <=
            ytec::uisupport::kMaximumErrorNextActionCharacters,
        "next action exceeded its public bound");
    check(
        primary.size() <= ytec::uisupport::kMaximumErrorPrimaryCharacters,
        "primary text exceeded its public bound");
    check_not_contains(
        primary,
        error.operation,
        "raw operation leaked into always-visible primary text");
    check_not_contains(
        primary,
        error.message,
        "raw message leaked into always-visible primary text");
    check(
        primary.find(ytec::clonecore::error_code_name(code)) !=
            std::wstring::npos,
        "stable engine code was not included in primary text");
  }
}

void test_secret_path_and_stack_are_removed_from_details() {
  const ytec::clonecore::Error error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = L"PowerShell D:\\Private\\customer-document.txt",
      .message =
          L"password=CorrectHorseBatteryStaple\r\n"
          L"recovery_key: 111111-222222-333333\r\n"
          L"Authorization: Bearer server-issued-access-value\r\n"
          L"Cookie: session=private-browser-session\r\n"
          L"C:\\Users\\Example\\Documents\\customer-name.txt failed\r\n"
          L"at Product.Engine.CopySecret()\r\n"
          L"FullyQualifiedErrorId: UnauthorizedAccess\r\n"
          L"読み書きを完了できませんでした",
  };
  const auto presentation =
      ytec::uisupport::make_error_presentation(error);
  const std::wstring primary =
      ytec::uisupport::format_error_primary(presentation);
  const std::wstring copy =
      ytec::uisupport::format_error_details_for_copy(presentation);

  for (const std::wstring* value : {&primary, &presentation.details, &copy}) {
    check_not_contains(
        *value,
        L"CorrectHorseBatteryStaple",
        "password value leaked into presentation text");
    check_not_contains(
        *value,
        L"111111-222222-333333",
        "recovery key leaked into presentation text");
    check_not_contains(
        *value,
        L"server-issued-access-value",
        "authorization value leaked into presentation text");
    check_not_contains(
        *value,
        L"private-browser-session",
        "cookie value leaked into presentation text");
    check_not_contains(
        *value,
        L"customer-document.txt",
        "operation path leaked into presentation text");
    check_not_contains(
        *value,
        L"customer-name.txt",
        "message path leaked into presentation text");
    check_not_contains(
        *value,
        L"CopySecret",
        "stack frame leaked into presentation text");
    check_not_contains(
        *value,
        L"UnauthorizedAccess",
        "PowerShell stack metadata leaked into presentation text");
  }
  check(
      presentation.details.find(L"秘密値") != std::wstring::npos,
      "secret omission was not made visible in expandable details");
  check(
      presentation.details.find(L"パスを含む行") != std::wstring::npos,
      "path omission was not made visible in expandable details");
  check(
      presentation.details.find(L"読み書きを完了できませんでした") !=
          std::wstring::npos,
      "safe diagnostic message was removed with unsafe details");
}

void test_huge_detail_and_copy_payload_are_bounded() {
  std::wstring huge = L"通常の先頭情報\r\n";
  for (std::size_t index = 0U; index < 5000U; ++index) {
    huge += L"at Product.Namespace.Component.Function()\r\n";
  }
  huge += std::wstring(100000U, L'X');
  const auto presentation = ytec::uisupport::make_error_presentation({
      .code = ytec::clonecore::ErrorCode::internal_error,
      .native_code = ERROR_UNHANDLED_EXCEPTION,
      .operation = huge,
      .message = huge,
  });
  const std::wstring copy =
      ytec::uisupport::format_error_details_for_copy(presentation);
  check(
      presentation.details.size() <=
          ytec::uisupport::kMaximumErrorDetailCharacters,
      "expandable details exceeded their public bound");
  check(
      copy.size() <= ytec::uisupport::kMaximumErrorCopyCharacters,
      "copy payload exceeded its public bound");
  check(
      presentation.details.find(L"詳細の一部を省略") !=
          std::wstring::npos,
      "truncated stack did not report omission");
  check_not_contains(
      copy,
      L"Product.Namespace.Component.Function",
      "huge stack leaked into copied details");
}

void test_native_code_is_stable_hex() {
  const auto presentation = ytec::uisupport::make_error_presentation({
      .code = ytec::clonecore::ErrorCode::verification_failed,
      .native_code = 0x80070017U,
      .operation = L"読戻し検証",
      .message = L"Hashが一致しません",
  });
  check(
      presentation.code ==
          L"verification_failed / native=0x80070017",
      "native code was not formatted as stable fixed-width hex");
  check(
      presentation.details.find(L"Hashが一致しません") !=
          std::wstring::npos,
      "safe detail was not retained");
}

void test_formatter_rebounds_untrusted_model_fields() {
  ytec::uisupport::ErrorPresentation presentation{
      .summary = std::wstring(5000U, L'S'),
      .code = std::wstring(5000U, L'C'),
      .next_action = std::wstring(5000U, L'N'),
      .details = std::wstring(10000U, L'D'),
      .details_expandable = true,
      .details_copyable = true,
  };
  const std::wstring primary =
      ytec::uisupport::format_error_primary(presentation);
  const std::wstring copy =
      ytec::uisupport::format_error_details_for_copy(presentation);
  check(
      primary.size() <= ytec::uisupport::kMaximumErrorPrimaryCharacters,
      "formatter trusted an oversized model primary field");
  check(
      copy.size() <= ytec::uisupport::kMaximumErrorCopyCharacters,
      "formatter trusted an oversized model detail field");
}

void test_formatter_rejects_tampered_sensitive_model_fields() {
  ytec::uisupport::ErrorPresentation presentation{
      .summary = L"C:\\Users\\Customer\\secret.txt",
      .code = L"password=visible-in-primary",
      .next_action = L"at Product.Engine.LeakSecret()",
      .details =
          L"Authorization: Bearer copied-access-value\r\n"
          L"安全な補足情報",
      .details_expandable = true,
      .details_copyable = true,
  };
  const std::wstring primary =
      ytec::uisupport::format_error_primary(presentation);
  const std::wstring copy =
      ytec::uisupport::format_error_details_for_copy(presentation);
  check(
      primary.find(L"処理を続けられませんでした") != std::wstring::npos,
      "tampered summary did not fall back to safe primary text");
  check(
      primary.find(L"internal_error") != std::wstring::npos,
      "tampered code did not fall back to the stable internal code");
  check_not_contains(
      primary,
      L"Customer",
      "tampered path leaked into primary text");
  check_not_contains(
      primary,
      L"visible-in-primary",
      "tampered secret leaked into primary text");
  check_not_contains(
      primary,
      L"LeakSecret",
      "tampered stack frame leaked into primary text");
  check_not_contains(
      copy,
      L"copied-access-value",
      "tampered secret leaked into copied details");
  check(
      copy.find(L"安全な補足情報") != std::wstring::npos,
      "safe copied detail was removed with tampered secret text");
}

void test_formatters_are_idempotent_and_do_not_mutate_input() {
  const ytec::uisupport::ErrorPresentation presentation{
      .summary = L"C:\\Users\\Customer\\secret.txt",
      .code = L"password=visible-in-primary",
      .next_action = L"at Product.Engine.LeakSecret()",
      .details =
          L"Authorization: Bearer copied-access-value\r\n"
          L"安全な補足情報",
      .details_expandable = true,
      .details_copyable = true,
  };
  const ytec::uisupport::ErrorPresentation original = presentation;
  const std::wstring first_primary =
      ytec::uisupport::format_error_primary(presentation);
  const std::wstring second_primary =
      ytec::uisupport::format_error_primary(presentation);
  const std::wstring first_copy =
      ytec::uisupport::format_error_details_for_copy(presentation);
  const std::wstring second_copy =
      ytec::uisupport::format_error_details_for_copy(presentation);

  check(first_primary == second_primary, "primary formatter was not idempotent");
  check(first_copy == second_copy, "copy formatter was not idempotent");
  check(
      presentation.summary == original.summary &&
          presentation.code == original.code &&
          presentation.next_action == original.next_action &&
          presentation.details == original.details &&
          presentation.details_expandable == original.details_expandable &&
          presentation.details_copyable == original.details_copyable,
      "formatter mutated the public presentation model");
}

}  // namespace

int main() {
  test_all_codes_have_bounded_primary_text();
  test_secret_path_and_stack_are_removed_from_details();
  test_huge_detail_and_copy_payload_are_bounded();
  test_native_code_is_stable_hex();
  test_formatter_rebounds_untrusted_model_fields();
  test_formatter_rejects_tampered_sensitive_model_fields();
  test_formatters_are_idempotent_and_do_not_mutate_input();
  std::cout << "ui support error presentation tests: PASS\n";
  return 0;
}

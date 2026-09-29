#include "ytec/clonecore/log.h"
#include "ytec/clonecore/log_privacy.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void credentials_redact_the_complete_unstructured_record() {
  const std::vector<std::wstring> private_records{
      L"password=Correct-Horse-Battery-Staple",
      L"passwd=private-password-alias",
      L"pwd: private-short-password-key",
      L"pwd \t: private-tabbed-password-key",
      L"TOKEN: opaque-access-token",
      L"token value=private-token-value",
      L"token\t=private-tab-separated-token",
      L"token\u3000=private-unicode-space-token",
      L"token\u00a0=private-nbsp-token",
      L"パスワード\u3000：private-fullwidth-spaced-password",
      L"recovery-key=111111-222222-333333-444444",
      L"recovery\tkey=private-tab-separated-recovery-key",
      L"recoveryKey=private-camel-recovery-key",
      L"{\"imagePassword\":\"private-compound-password\"}",
      L"{\"bitlockerRecoveryKey\":\"private-compound-key\"}",
      L"{\"sessionCookie\":\"private-compound-cookie\"}",
      L"{\"accessToken\":\"private-json-token\"}",
      L"{\"refreshToken\":\"private-json-token\"}",
      L"{\"authHeader\":\"Basic private-json-auth\"}",
      L"Cookie: session=private-cookie",
      L"Authorization: Bearer private-bearer-value",
      L"Authorization \t: Basic private-basic-value",
      L"auth_header=Basic private-basic-value",
      L"auth\theader=Basic private-tab-separated-auth",
      L"X-Api-Key=private-api-key",
      L"client_secret=private-client-secret",
      L"credential=private-credential",
      L"パスワード=private-password",
      L"password value private-password-value",
      L"回復キー=private-recovery-key",
  };
  for (const auto& record : private_records) {
    const auto sanitized =
        ytec::clonecore::sanitize_main_log_message(record);
    check(sanitized == L"[PRIVATE]",
          "Credential-bearing records must fail closed as one marker");
    check(sanitized.find(record) == std::wstring::npos,
          "No complete credential-bearing input may survive");
  }

  std::wstring boundary_record(28U, L'a');
  boundary_record += L" password=secret-crossing-limit";
  check(ytec::clonecore::sanitize_main_log_message(boundary_record, 32U) ==
            L"[PRIVATE]",
        "A sensitive marker crossing the output bound must redact all");
  std::wstring long_record(100000U, L'a');
  long_record += L" password=secret-beyond-inspection";
  const auto bounded =
      ytec::clonecore::sanitize_main_log_message(long_record, 32U);
  check(bounded.size() == 32U &&
            bounded.find(L"secret") == std::wstring::npos,
        "Input beyond the bounded inspection window must never reach output");
}

void paths_and_document_names_are_not_copied() {
  const std::vector<std::wstring> absolute_paths{
      L"open C:\\Users\\Alice\\Documents\\Private Budget.xlsx failed",
      L"open C:/Users/Alice/Documents/private.pdf failed",
      L"open \\\\server\\share\\Customer List.docx failed",
      L"open \\\\?\\C:\\Customers\\private.csv failed",
      L"open \\Device\\HarddiskVolume3\\Customers\\private.txt failed",
  };
  for (const auto& record : absolute_paths) {
    check(ytec::clonecore::sanitize_main_log_message(record) == L"[PATH]",
          "Absolute paths must be removed as a whole");
  }
  check(ytec::clonecore::sanitize_main_log_message(
            L"document=Customer-List-2026.pdf") == L"[DOCUMENT]",
        "A standalone document name must be removed");
  check(ytec::clonecore::sanitize_main_log_message(
            L"document=Customer List") == L"[DOCUMENT]",
        "A labeled extension-free document name must be removed");
  check(ytec::clonecore::sanitize_main_log_message(
            L"文書=顧客一覧") == L"[DOCUMENT]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"ファイル名：顧客一覧") == L"[DOCUMENT]",
        "Japanese extension-free document labels must be removed");
  check(ytec::clonecore::sanitize_main_log_message(
            L"文書=顧客一覧.pdfを開けません") == L"[DOCUMENT]",
        "A Japanese document name must be removed before following text");
  check(ytec::clonecore::sanitize_main_log_message(
            L"復元開始 format=tsumugi stage=verify") ==
            L"復元開始 format=tsumugi stage=verify",
        "A safe extension-free operation message should remain useful");
  check(ytec::clonecore::sanitize_main_log_message(
            L"Windows直接.tsumugi復元を開始") ==
            L"Windows直接.tsumugi復元を開始",
        "A format name must not be mistaken for a document name");
  check(ytec::clonecore::sanitize_main_log_message(
            L"elapsed=1.5ms version=1.0rc1") ==
            L"elapsed=1.5ms version=1.0rc1",
        "Numeric timings and versions must not be document names");
  check(ytec::clonecore::sanitize_main_log_message(
            L"update_url=https://ytec.example.invalid/check") ==
            L"update_url=https://ytec.example.invalid/check",
        "An HTTPS URL must not be mistaken for a local absolute path");
  check(ytec::clonecore::sanitize_main_log_message(
            L"cancellation token requested") ==
            L"cancellation token requested",
        "A token control-object status must not imply a credential value");
  check(ytec::clonecore::sanitize_main_log_message(
            L"password buffer cleared") == L"password buffer cleared" &&
            ytec::clonecore::sanitize_main_log_message(
                L"document_name_policy=disabled") ==
                L"document_name_policy=disabled" &&
            ytec::clonecore::sanitize_main_log_message(
                L"serial_number_available=false") ==
                L"serial_number_available=false",
        "Value-free privacy state diagnostics must remain useful");
  check(ytec::clonecore::sanitize_main_log_message(
            L"open \\Users\\Alice\\private failed") == L"[PATH]",
        "A Windows rooted path must be removed");
  check(ytec::clonecore::sanitize_main_log_message(
            L"保存先：C:/Users/Alice/private") == L"[PATH]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"保存先：\\Users\\Alice\\private") == L"[PATH]",
        "Unicode-delimited Windows paths must be removed");
  check(ytec::clonecore::sanitize_main_log_message(
            L"open /Users/Alice/Private failed") == L"[PATH]",
        "A forward-slash rooted Windows path must be removed");
  check(ytec::clonecore::sanitize_main_log_message(
            L"open Customer.md failed") == L"[DOCUMENT]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"open Customer.html failed") == L"[DOCUMENT]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"open Customer.epub failed") == L"[DOCUMENT]",
        "Common document extensions must not leave names in logs");
  check(ytec::clonecore::sanitize_main_log_message(
            L"auth header policy=disabled") ==
            L"auth header policy=disabled" &&
            ytec::clonecore::sanitize_main_log_message(
                L"private keyboard layout selected") ==
                L"private keyboard layout selected" &&
            ytec::clonecore::sanitize_main_log_message(
                L"proxy-authorization support disabled") ==
                L"proxy-authorization support disabled",
        "Value-free header and keyboard diagnostics must remain useful");
  check(ytec::clonecore::sanitize_main_log_message(
            L"password is required") == L"password is required" &&
            ytec::clonecore::sanitize_main_log_message(
                L"auth header is absent") == L"auth header is absent" &&
            ytec::clonecore::sanitize_main_log_message(
                L"bearer authentication disabled") ==
                L"bearer authentication disabled",
        "Natural-language privacy state diagnostics must remain useful");
  const std::vector<std::wstring> command_switches{
      L"option=/quiet",
      L"BCDBoot /f UEFI",
      L"DISM /Online",
      L"analysis /WX enabled",
  };
  for (const auto& record : command_switches) {
    check(ytec::clonecore::sanitize_main_log_message(record) == record,
          "A Windows command-line switch must not be treated as a path");
  }
}

void disk_and_device_identifiers_are_minimized_with_domain_hashes() {
  using ytec::clonecore::MainLogPrivateValueKind;
  const std::wstring serial = L"SN-1234567890";
  const std::wstring device = L"USBSTOR\\DISK&VEN_TEST&PROD_PRIVATE\\ABC123";
  const auto first = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::disk_serial, serial);
  const auto second = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::disk_serial, serial);
  const auto device_token = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::device_instance_id, device);

  check(first == second && first.starts_with(L"[DISK#") &&
            first.ends_with(L"~7890]") && first.size() == 76U,
        "Disk serials need a stable SHA-256 token and four-char suffix");
  check(std::all_of(
            first.begin() + 6,
            first.begin() + 70,
            [](const wchar_t value) {
              return (value >= L'0' && value <= L'9') ||
                     (value >= L'a' && value <= L'f');
            }),
        "The disk identifier must contain a lowercase 64-digit SHA-256");
  check(first.substr(6U, 64U) ==
            L"767a3095df4dcfcffeb0bcdf92597f6e25e1a1883603a46ca41dbc38ac8d1ef0",
        "The disk identifier hash must match the fixed UTF-16LE vector");
  check(first.find(serial) == std::wstring::npos,
        "A complete disk serial must not appear in its safe token");
  check(device_token.starts_with(L"[DEVICE#") &&
            device_token.ends_with(L"]") && device_token.size() == 73U &&
            device_token.find(device) == std::wstring::npos,
        "Device instance IDs need a hash-only token");
  check(first != device_token,
        "Disk and device identifiers must use separate hash domains");
  const auto different_serial = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::disk_serial, L"SN-1234567891");
  check(different_serial.substr(6U, 64U) != first.substr(6U, 64U),
        "Different serials must not receive a constant digest");
  const auto same_value_device = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::device_instance_id, serial);
  check(same_value_device.substr(8U, 64U) ==
            L"1fa8cff2db0cbdb183e545fb01347c9c28e38f728f334c79029e5ee28577b207" &&
            same_value_device.substr(8U, 64U) != first.substr(6U, 64U),
        "The device domain must match its vector and differ from disk serials");
  const auto structured_record =
      ytec::clonecore::sanitize_main_log_message(
          L"disk_id=" + first + L" bytes=1024");
  check(structured_record == L"disk_id=" + first + L" bytes=1024",
        "An already-minimized disk token must retain diagnostic value");
  const auto short_serial = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::disk_serial, L"ABCD");
  check(short_serial.find(L"ABCD") == std::wstring::npos &&
            short_serial.size() == 71U,
        "A short serial must not expose its complete value as a suffix");
  const auto nearly_short_serial =
      ytec::clonecore::minimize_main_log_value(
          MainLogPrivateValueKind::disk_serial, L"ABCDE");
  check(nearly_short_serial.find(L"BCDE") == std::wstring::npos &&
            nearly_short_serial.size() == 71U,
        "A five-character serial must remain hash-only");
  const auto unsafe_suffix = ytec::clonecore::minimize_main_log_value(
      MainLogPrivateValueKind::disk_serial, L"SERIAL-ABC]");
  check(unsafe_suffix.find(L"ABC]") == std::wstring::npos &&
            unsafe_suffix.size() == 71U,
        "A serial suffix with syntax characters must be hash-only");

  const auto unstructured_serial =
      ytec::clonecore::sanitize_main_log_message(
          L"disk serial=SN-1234567890 bytes=1024");
  check(unstructured_serial == L"[DISK]",
        "An unstructured serial assignment must fail closed as a whole");
  check(ytec::clonecore::sanitize_main_log_message(
            L"disk serial: ACME SN123456 bytes=1024") == L"[DISK]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"{\"serial\" \t: \"SN123456\"}") == L"[DISK]",
        "Colon, spaced, and JSON serial forms must not leak partial values");
  check(ytec::clonecore::sanitize_main_log_message(
            L"disk serial_suffix=12345678 bytes=1024") == L"[DISK]",
        "A pre-shortened suffix must not be trusted as a complete serial");
  check(ytec::clonecore::sanitize_main_log_message(
            L"serial number=SN-1234567890") == L"[DISK]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"{\"serial number\":\"SN-1234567890\"}") == L"[DISK]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"disk serial no=SN-1234567890") == L"[DISK]",
        "Space-delimited serial labels must not expose disk serials");
  check(ytec::clonecore::sanitize_main_log_message(
            L"{\"diskSerial\":\"SN123456\"}") == L"[DISK]" &&
            ytec::clonecore::sanitize_main_log_message(
                L"{\"diskSerialNumber\":\"SN123456\"}") == L"[DISK]",
        "Compound camel-case serial keys must not expose disk serials");
  check(ytec::clonecore::sanitize_main_log_message(
            L"device USBSTOR\\DISK&VEN_TEST\\ABC123") == L"[DEVICE]",
        "A raw device-instance form must be removed defensively");
  check(ytec::clonecore::sanitize_main_log_message(
            L"device_instance_id: ACME DEVICE 123") == L"[DEVICE]",
        "A colon and spaced device instance ID must fail closed as a whole");
}

void logger_entry_point_always_applies_privacy_and_line_bounds() {
  std::vector<ytec::clonecore::LogRecord> observed;
  const ytec::clonecore::Logger logger(
      [&observed](const ytec::clonecore::LogRecord& record) {
        observed.push_back(record);
      });
  logger.info(L"Authorization: Bearer should-never-reach-sink");
  logger.warning(L"safe line\nwith tab\tand control");

  check(observed.size() == 2U && observed.front().message == L"[PRIVATE]",
        "Every Logger sink must receive the centrally sanitized record");
  check(observed.back().message == L"safe line with tab and control" &&
            observed.back().message.find_first_of(L"\r\n\t") ==
                std::wstring::npos,
        "Logger sinks must receive bounded single-line text");

  const auto bounded = ytec::clonecore::sanitize_main_log_message(
      L"0123456789", 8U);
  check(bounded == L"01234..." && bounded.size() == 8U,
        "The sanitizer must respect its exact output bound");
  std::wstring surrogate_boundary(12U, L'a');
  surrogate_boundary.push_back(static_cast<wchar_t>(0xd83dU));
  surrogate_boundary.push_back(static_cast<wchar_t>(0xde00U));
  surrogate_boundary += L"xyz";
  const auto surrogate_safe =
      ytec::clonecore::sanitize_main_log_message(surrogate_boundary, 16U);
  check(surrogate_safe == L"aaaaaaaaaaaa...",
        "Truncation must remove a complete surrogate pair, not split it");
  check(ytec::clonecore::sanitize_main_log_message(L"safe\nrecord") ==
            ytec::clonecore::sanitize_main_log_message(
                ytec::clonecore::sanitize_main_log_message(L"safe\nrecord")),
        "Sanitization should be idempotent for safe output");
}

void structured_non_identifier_values_use_fixed_markers() {
  using ytec::clonecore::MainLogPrivateValueKind;
  check(ytec::clonecore::minimize_main_log_value(
            MainLogPrivateValueKind::secret, L"private") == L"[PRIVATE]",
        "Secrets must use a fixed marker");
  check(ytec::clonecore::minimize_main_log_value(
            MainLogPrivateValueKind::absolute_path,
            L"C:\\Users\\Alice\\private.pdf") == L"[PATH]",
        "Absolute paths must use a fixed marker");
  check(ytec::clonecore::minimize_main_log_value(
            MainLogPrivateValueKind::document_name,
            L"private.pdf") == L"[DOCUMENT]",
        "Document names must use a fixed marker");
}

}  // namespace

int main() {
  try {
    credentials_redact_the_complete_unstructured_record();
    paths_and_document_names_are_not_copied();
    disk_and_device_identifiers_are_minimized_with_domain_hashes();
    logger_entry_point_always_applies_privacy_and_line_bounds();
    structured_non_identifier_values_use_fixed_markers();
    std::cout << "log privacy tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "log privacy tests: FAIL: " << exception.what() << '\n';
    return 1;
  }
}

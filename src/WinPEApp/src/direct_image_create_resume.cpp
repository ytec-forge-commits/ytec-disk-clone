#include "ytec/winpeapp/direct_image_create_resume.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::winpeapp {
namespace {

constexpr std::wstring_view kContinuityPrefix =
    L"YTEC-PE-EXACT-IMAGE-CREATE-CONTINUITY-V1|";
constexpr std::size_t kMaximumContinuityCharacters = 512U;
constexpr std::size_t kMaximumCreatedUtcCharacters = 64U;
constexpr std::size_t kMaximumAppVersionCharacters = 128U;
constexpr std::size_t kPartitionSelectionBitmapBytes = 16U;

clonecore::Error resume_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return {
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
  return clonecore::Result<T>::failure(resume_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool printable_ascii(
    const std::string_view value,
    const std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
      std::all_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte >= 0x20U && byte <= 0x7eU;
      });
}

bool canonical_partition_numbers(
    const std::vector<std::uint32_t>& values) noexcept {
  return values.size() <= 128U &&
      std::is_sorted(values.begin(), values.end()) &&
      std::adjacent_find(values.begin(), values.end()) == values.end() &&
      std::all_of(values.begin(), values.end(), [](const auto value) {
        return value >= 1U && value <= 128U;
      });
}

std::array<std::byte, kPartitionSelectionBitmapBytes>
partition_selection_bitmap(
    const std::vector<std::uint32_t>& values) noexcept {
  std::array<std::byte, kPartitionSelectionBitmapBytes> result{};
  for (const auto value : values) {
    const std::size_t bit = static_cast<std::size_t>(value - 1U);
    result[bit / 8U] |= static_cast<std::byte>(1U << (bit % 8U));
  }
  return result;
}

std::vector<std::uint32_t> partition_numbers_from_bitmap(
    const std::array<std::byte, kPartitionSelectionBitmapBytes>& bitmap) {
  std::vector<std::uint32_t> result;
  result.reserve(128U);
  for (std::uint32_t number = 1U; number <= 128U; ++number) {
    const std::size_t bit = static_cast<std::size_t>(number - 1U);
    if ((std::to_integer<unsigned int>(bitmap[bit / 8U]) &
         (1U << (bit % 8U))) != 0U) {
      result.push_back(number);
    }
  }
  return result;
}

wchar_t hex_digit(const unsigned int value) noexcept {
  return static_cast<wchar_t>(
      value < 10U ? L'0' + value : L'a' + (value - 10U));
}

template <std::size_t Size>
void append_hex(
    std::wstring& output,
    const std::array<std::byte, Size>& value) {
  output.reserve(output.size() + Size * 2U);
  for (const std::byte byte : value) {
    const auto number = std::to_integer<unsigned int>(byte);
    output.push_back(hex_digit(number >> 4U));
    output.push_back(hex_digit(number & 0x0fU));
  }
}

int hex_value(const wchar_t value) noexcept {
  if (value >= L'0' && value <= L'9') {
    return static_cast<int>(value - L'0');
  }
  if (value >= L'a' && value <= L'f') {
    return static_cast<int>(value - L'a') + 10;
  }
  return -1;
}

template <std::size_t Size>
bool read_hex(
    const std::wstring_view value,
    std::array<std::byte, Size>& result) noexcept {
  if (value.size() != Size * 2U) {
    return false;
  }
  for (std::size_t index = 0U; index < Size; ++index) {
    const int high = hex_value(value[index * 2U]);
    const int low = hex_value(value[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    result[index] = static_cast<std::byte>(
        static_cast<unsigned int>((high << 4) | low));
  }
  return true;
}

void append_ascii(std::wstring& output, const std::string_view value) {
  output.reserve(output.size() + value.size());
  for (const unsigned char byte : value) {
    output.push_back(static_cast<wchar_t>(byte));
  }
}

bool parse_decimal(
    const std::wstring_view value,
    std::size_t& number) noexcept {
  if (value.empty() || (value.size() > 1U && value.front() == L'0')) {
    return false;
  }
  std::size_t parsed{};
  for (const wchar_t character : value) {
    if (character < L'0' || character > L'9') {
      return false;
    }
    const std::size_t digit = static_cast<std::size_t>(character - L'0');
    if (parsed > ((std::numeric_limits<std::size_t>::max)() - digit) / 10U) {
      return false;
    }
    parsed = parsed * 10U + digit;
  }
  number = parsed;
  return true;
}

bool consume_literal(
    const std::wstring_view input,
    std::size_t& offset,
    const std::wstring_view literal) noexcept {
  if (literal.size() > input.size() - offset ||
      input.substr(offset, literal.size()) != literal) {
    return false;
  }
  offset += literal.size();
  return true;
}

bool consume_length_ascii(
    const std::wstring_view input,
    std::size_t& offset,
    const std::size_t maximum,
    std::string& result) {
  const std::size_t colon = input.find(L':', offset);
  if (colon == std::wstring_view::npos) {
    return false;
  }
  std::size_t length{};
  if (!parse_decimal(input.substr(offset, colon - offset), length) ||
      length == 0U || length > maximum ||
      length > input.size() - (colon + 1U)) {
    return false;
  }
  result.clear();
  result.reserve(length);
  for (std::size_t index = 0U; index < length; ++index) {
    const wchar_t character = input[colon + 1U + index];
    if (character < 0x20 || character > 0x7e) {
      return false;
    }
    result.push_back(static_cast<char>(character));
  }
  offset = colon + 1U + length;
  return true;
}

bool same_binding(
    const operationcore::ResumeSlotBinding& left,
    const operationcore::ResumeSlotBinding& right) noexcept {
  if (left.capability != right.capability ||
      left.operation_id != right.operation_id ||
      left.identities.source_identity_hash !=
          right.identities.source_identity_hash ||
      left.identities.target_identity_hash !=
          right.identities.target_identity_hash ||
      left.identities.output_identity_hash !=
          right.identities.output_identity_hash ||
      left.checkpoint_record_hash != right.checkpoint_record_hash ||
      left.partial_file_object_identity_hash !=
          right.partial_file_object_identity_hash ||
      left.owned_object_file_bindings.size() !=
          right.owned_object_file_bindings.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < left.owned_object_file_bindings.size(); ++index) {
    const auto& left_object = left.owned_object_file_bindings[index];
    const auto& right_object = right.owned_object_file_bindings[index];
    if (left_object.role != right_object.role ||
        left_object.file_object_identity_hash !=
            right_object.file_object_identity_hash) {
      return false;
    }
  }
  return true;
}

}  // namespace

clonecore::Result<std::wstring>
build_direct_image_create_resume_continuity_v1(
    const DirectImageCreateResumeContinuityV1& continuity) {
  if (!printable_ascii(
          continuity.created_utc, kMaximumCreatedUtcCharacters) ||
      !printable_ascii(
          continuity.app_version, kMaximumAppVersionCharacters) ||
      !imageformat::is_supported_tsumugi_create_verification_mode(
          continuity.verification_mode) ||
      !canonical_partition_numbers(
          continuity.selected_partition_numbers) ||
      all_zero(continuity.image_id) ||
      continuity.argon2.memory_kib !=
          imageformat::kTsumugiArgon2MemoryKiB ||
      continuity.argon2.iterations !=
          imageformat::kTsumugiArgon2Iterations ||
      continuity.argon2.parallelism !=
          imageformat::kTsumugiArgon2Parallelism ||
      (continuity.encrypted != !all_zero(continuity.argon2.salt)) ||
      (continuity.encrypted != !all_zero(continuity.base_nonce))) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"PE image-create continuity作成",
        L"非秘密field、固定暗号profile、またはrandom識別子が不正です");
  }

  std::wstring token(kContinuityPrefix);
  token += std::to_wstring(continuity.created_utc.size());
  token.push_back(L':');
  append_ascii(token, continuity.created_utc);
  token.push_back(L'|');
  token += std::to_wstring(continuity.app_version.size());
  token.push_back(L':');
  append_ascii(token, continuity.app_version);
  token += L"|V";
  token.push_back(
      continuity.verification_mode ==
              imageformat::TsumugiCreateVerificationMode::complete
          ? L'0'
          : L'1');
  token += L"|E";
  token.push_back(continuity.encrypted ? L'1' : L'0');
  token += L"|I";
  append_hex(token, continuity.image_id);
  token += L"|M65536|T3|P1|S";
  append_hex(token, continuity.argon2.salt);
  token += L"|N";
  append_hex(token, continuity.base_nonce);
  if (!continuity.selected_partition_numbers.empty()) {
    token += L"|Q";
    append_hex(
        token,
        partition_selection_bitmap(
            continuity.selected_partition_numbers));
  }
  if (token.size() > kMaximumContinuityCharacters) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BUFFER_OVERFLOW,
        L"PE image-create continuity上限",
        L"canonical continuityがcheckpoint固定上限を超えます");
  }
  return clonecore::Result<std::wstring>::success(std::move(token));
}

clonecore::Result<DirectImageCreateResumeContinuityV1>
parse_direct_image_create_resume_continuity_v1(
    const std::wstring& token) {
  if (token.size() > kMaximumContinuityCharacters ||
      !std::wstring_view(token).starts_with(kContinuityPrefix)) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_REVISION_MISMATCH,
        L"PE image-create continuity読取り",
        L"versionまたは固定サイズ上限が不正です");
  }
  const std::wstring_view input(token);
  std::size_t offset = kContinuityPrefix.size();
  DirectImageCreateResumeContinuityV1 result;
  if (!consume_length_ascii(
          input,
          offset,
          kMaximumCreatedUtcCharacters,
          result.created_utc) ||
      !consume_literal(input, offset, L"|") ||
      !consume_length_ascii(
          input,
          offset,
          kMaximumAppVersionCharacters,
          result.app_version) ||
      !consume_literal(input, offset, L"|V") || offset >= input.size() ||
      (input[offset] != L'0' && input[offset] != L'1')) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create continuity field",
        L"長さ付きASCII fieldまたは検証方式が不正です");
  }
  result.verification_mode = input[offset++] == L'0'
      ? imageformat::TsumugiCreateVerificationMode::complete
      : imageformat::TsumugiCreateVerificationMode::fast;
  if (!consume_literal(input, offset, L"|E") || offset >= input.size() ||
      (input[offset] != L'0' && input[offset] != L'1')) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create continuity暗号化field",
        L"暗号化classificationが不正です");
  }
  result.encrypted = input[offset++] == L'1';
  if (!consume_literal(input, offset, L"|I") ||
      32U > input.size() - offset ||
      !read_hex(input.substr(offset, 32U), result.image_id)) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create continuity image ID",
        L"image IDがcanonical lowercase hexではありません");
  }
  offset += 32U;
  if (!consume_literal(input, offset, L"|M65536|T3|P1|S") ||
      32U > input.size() - offset ||
      !read_hex(input.substr(offset, 32U), result.argon2.salt)) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create continuity Argon2",
        L"固定Argon2 profileまたはSaltが不正です");
  }
  offset += 32U;
  if (!consume_literal(input, offset, L"|N") ||
      imageformat::kTsumugiGcmNonceBytes * 2U > input.size() - offset ||
      !read_hex(
          input.substr(
              offset, imageformat::kTsumugiGcmNonceBytes * 2U),
          result.base_nonce)) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create continuity Nonce",
        L"base Nonceまたは末尾長が不正です");
  }
  offset += imageformat::kTsumugiGcmNonceBytes * 2U;
  if (offset != input.size()) {
    std::array<std::byte, kPartitionSelectionBitmapBytes> bitmap{};
    if (!consume_literal(input, offset, L"|Q") ||
        kPartitionSelectionBitmapBytes * 2U != input.size() - offset ||
        !read_hex(
            input.substr(offset, kPartitionSelectionBitmapBytes * 2U),
            bitmap) ||
        all_zero(bitmap)) {
      return failure<DirectImageCreateResumeContinuityV1>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE image-create continuity partition選択",
          L"partition bitmapが空、非canonical、または末尾長不正です");
    }
    result.selected_partition_numbers =
        partition_numbers_from_bitmap(bitmap);
  }
  auto canonical = build_direct_image_create_resume_continuity_v1(result);
  if (!canonical || canonical.value() != token) {
    return failure<DirectImageCreateResumeContinuityV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create continuity canonical照合",
        L"非canonical、秘密field混入、またはfield関係不一致を拒否しました");
  }
  return clonecore::Result<DirectImageCreateResumeContinuityV1>::success(
      std::move(result));
}

clonecore::Result<DirectImageCreateResumeStartupObservation>
inspect_direct_image_create_resume_v1(
    operationcore::IResumeSlotPlatform& platform) {
  auto inspected = platform.inspect_persistent_pe_exact_image_create();
  if (!inspected) {
    return clonecore::Result<
        DirectImageCreateResumeStartupObservation>::failure(
        inspected.error());
  }
  DirectImageCreateResumeStartupObservation result{
      .object_state = inspected.value().state,
      .binding = inspected.value().binding,
      .final_path = inspected.value().final_path,
  };
  if (inspected.value().state ==
          operationcore::PersistentPeExactImageCreateObjectState::no_slot ||
      inspected.value().state == operationcore::
          PersistentPeExactImageCreateObjectState::other_capability) {
    return clonecore::Result<
        DirectImageCreateResumeStartupObservation>::success(
        std::move(result));
  }
  if (!inspected.value().slot || !inspected.value().binding) {
    return failure<DirectImageCreateResumeStartupObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE image-create startup slot shape",
        L"persistent image-create classificationにslotと完全bindingがありません");
  }
  const auto& checkpoint =
      inspected.value().slot->checkpoint.checkpoint;
  auto continuity = parse_direct_image_create_resume_continuity_v1(
      checkpoint.continuity_token);
  if (!continuity || !checkpoint.source || checkpoint.target ||
      checkpoint.kind != operationcore::OperationKind::image_create ||
      checkpoint.environment != operationcore::OperationEnvironment::winpe ||
      checkpoint.schema_version !=
          operationcore::kCheckpointSchemaVersionV3 ||
      !checkpoint.output_progress_evidence) {
    return continuity
        ? failure<DirectImageCreateResumeStartupObservation>(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"PE image-create startup checkpoint",
              L"source、schema v3、phase、または出力進捗証跡が不正です")
        : clonecore::Result<
              DirectImageCreateResumeStartupObservation>::failure(
              continuity.error());
  }
  result.source = checkpoint.source;
  result.verified_logical_bytes = checkpoint.verified_work_bytes;
  result.verified_chunk_count = checkpoint.verified_chunk_count;
  result.expected_logical_bytes = checkpoint.expected_work_bytes;
  result.checkpoint_phase = checkpoint.phase;
  result.continuity = continuity.take_value();
  return clonecore::Result<
      DirectImageCreateResumeStartupObservation>::success(
      std::move(result));
}

clonecore::Result<std::wstring>
format_direct_image_create_resume_startup_review_v1(
    const DirectImageCreateResumeStartupObservation& observation) {
  if ((observation.object_state !=
           operationcore::PersistentPeExactImageCreateObjectState::staged &&
       observation.object_state !=
           operationcore::PersistentPeExactImageCreateObjectState::published &&
       observation.object_state != operationcore::
           PersistentPeExactImageCreateObjectState::retirement_pending) ||
      !observation.binding || !observation.source ||
      !observation.continuity || observation.final_path.empty() ||
      !observation.checkpoint_phase) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"PE image-create startup要約",
        L"表示可能なpersistent exact image-create観測ではありません");
  }
  const wchar_t* state = observation.object_state ==
          operationcore::PersistentPeExactImageCreateObjectState::staged
      ? L"作成途中"
      : observation.object_state ==
              operationcore::PersistentPeExactImageCreateObjectState::published
          ? L"完成名公開後・完全検証待ち"
          : L"完全検証後・再開情報整理待ち";
  std::wostringstream stream;
  stream << L"前回中断した通常イメージ作成があります。\r\n"
         << L"状態: " << state << L"\r\n"
         << L"作成元: " << observation.source->model << L" / "
         << observation.source->size_bytes << L" bytes\r\n"
         << L"出力: " << observation.final_path << L"\r\n"
         << L"確認済み: " << observation.verified_logical_bytes
         << L" / " << observation.expected_logical_bytes
         << L" bytes、" << observation.verified_chunk_count
         << L" chunks\r\n"
         << L"方式: WinPE exact永続再開 / "
         << (observation.continuity->verification_mode ==
                     imageformat::TsumugiCreateVerificationMode::complete
                 ? L"完全検証"
                 : L"高速選択（再開時は完全検証へ強化）")
         << L" / "
         << (observation.continuity->encrypted ? L"暗号化" : L"非暗号化")
         << L"\r\n対象: ";
  if (observation.continuity->selected_partition_numbers.empty()) {
    stream << L"ディスク全体";
  } else {
    for (std::size_t index = 0U;
         index < observation.continuity->selected_partition_numbers.size();
         ++index) {
      if (index != 0U) {
        stream << L", ";
      }
      stream << L"#"
             << observation.continuity->selected_partition_numbers[index];
    }
  }
  stream
         << L"\r\n\r\n"
         << L"再開には同じ作成元と保存先の再証明が必要です。"
            L"暗号化パスワードは保存していないため再入力してください。";
  return clonecore::Result<std::wstring>::success(stream.str());
}

clonecore::Status discard_direct_image_create_resume_v1(
    const operationcore::ResumeSlotBinding& reviewed_binding,
    operationcore::IResumeSlotPlatform& platform) {
  auto inspected = platform.inspect_persistent_pe_exact_image_create();
  if (!inspected) {
    return clonecore::Status::failure(inspected.error());
  }
  if (inspected.value().state !=
          operationcore::PersistentPeExactImageCreateObjectState::staged ||
      !inspected.value().binding ||
      !same_binding(*inspected.value().binding, reviewed_binding)) {
    return clonecore::Status::failure(resume_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE image-create staged破棄binding",
        L"表示後に状態が変化したか、公開済みfinalを破棄しようとしました"));
  }
  operationcore::SingleResumeSlot slot(platform);
  return slot.discard(reviewed_binding);
}

}  // namespace ytec::winpeapp

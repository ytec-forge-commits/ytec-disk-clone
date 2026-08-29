#include "ytec/windowsapp/resume_slot_product.h"

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/operationcore/windows_resume_slot_platform.h"
#include "ytec/windowsapp/startup_data_policy.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::array<std::byte, 13U> kAdmissionHashDomain{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'C'},
    std::byte{'W'}, std::byte{'I'}, std::byte{'N'}, std::byte{'A'},
    std::byte{'D'}, std::byte{'M'}, std::byte{'I'}, std::byte{'T'},
    std::byte{'1'},
};
constexpr std::array<std::byte, 15U> kBackingHashDomain{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'C'},
    std::byte{'W'}, std::byte{'I'}, std::byte{'N'}, std::byte{'S'},
    std::byte{'L'}, std::byte{'O'}, std::byte{'T'}, std::byte{'B'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'1'},
};
constexpr std::size_t kMaximumAdmissionTextFields = 16U;
constexpr std::size_t kMaximumAdmissionDigests = 16U;
constexpr std::size_t kMaximumAdmissionFieldCharacters = 32U * 1024U;
constexpr std::size_t kMaximumAdmissionCanonicalBytes = 256U * 1024U;
constexpr std::size_t kMaximumIdentityModelCharacters = 256U;
constexpr std::size_t kMaximumIdentitySerialCharacters = 128U;
constexpr std::size_t kMaximumIdentityDeviceCharacters = 1024U;
// disk number + size + sector size + system flag + three framed lengths.
constexpr std::size_t kIdentityCanonicalFixedBytes =
    4U + 8U + 4U + 1U + 8U + 8U + 8U;

clonecore::Error product_error(
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

clonecore::Error admission_error(
    std::wstring message,
    const clonecore::ErrorCode code = clonecore::ErrorCode::invalid_data,
    const DWORD native_code = ERROR_INVALID_DATA) {
  return product_error(
      code,
      native_code,
      L"Windows SingleResumeSlot admission plan",
      std::move(message));
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_utf16(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  append_u64(bytes, static_cast<std::uint64_t>(value.size()));
  for (const wchar_t character : value) {
    const auto code_unit = static_cast<std::uint16_t>(character);
    bytes.push_back(static_cast<std::byte>(code_unit & 0xFFU));
    bytes.push_back(static_cast<std::byte>((code_unit >> 8U) & 0xFFU));
  }
}

void append_ascii(
    std::vector<std::byte>& bytes,
    const std::string_view value) {
  append_u64(bytes, static_cast<std::uint64_t>(value.size()));
  if (!value.empty()) {
    bytes.insert(
        bytes.end(),
        reinterpret_cast<const std::byte*>(value.data()),
        reinterpret_cast<const std::byte*>(value.data() + value.size()));
  }
}

void append_identity(
    std::vector<std::byte>& bytes,
    const clonecore::StableDiskIdentity& identity) {
  append_u32(bytes, identity.disk_number);
  append_u64(bytes, identity.size_bytes);
  append_u32(bytes, identity.logical_sector_size);
  append_u8(bytes, identity.is_system_disk ? 1U : 0U);
  append_utf16(bytes, identity.model);
  append_ascii(bytes, identity.serial_suffix);
  append_utf16(bytes, identity.device_instance_id);
}

bool identity_matches(
    const clonecore::StableDiskIdentity& expected,
    const clonecore::StableDiskIdentity& observed) {
  return static_cast<bool>(clonecore::validate_stable_identity(
      expected, observed, L"Resume Slot data backing separation"));
}

bool checked_add_bounded(
    std::size_t& current,
    const std::size_t addition,
    const std::size_t maximum) noexcept {
  if (current > maximum || addition > maximum - current) {
    return false;
  }
  current += addition;
  return true;
}

bool checked_multiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::size_t>::max)() / left) {
    return false;
  }
  product = left * right;
  return true;
}

clonecore::Result<std::size_t> bounded_identity_canonical_size(
    const clonecore::StableDiskIdentity& identity,
    const std::wstring_view role) {
  const auto stable = clonecore::validate_stable_identity(
      identity, identity, role);
  if (!stable) {
    return clonecore::Result<std::size_t>::failure(stable.error());
  }
  if (identity.model.size() > kMaximumIdentityModelCharacters ||
      identity.serial_suffix.size() > kMaximumIdentitySerialCharacters ||
      identity.device_instance_id.size() >
          kMaximumIdentityDeviceCharacters ||
      identity.model.find(L'\0') != std::wstring::npos ||
      identity.device_instance_id.find(L'\0') != std::wstring::npos ||
      identity.serial_suffix.find('\0') != std::string::npos ||
      std::any_of(
          identity.serial_suffix.begin(),
          identity.serial_suffix.end(),
          [](const unsigned char character) {
            return character < 0x20U || character == 0x7FU;
          })) {
    return clonecore::Result<std::size_t>::failure(admission_error(
        std::wstring(role) +
        L"識別情報が安全上限を超えるか制御文字を含んでいます"));
  }

  std::size_t model_bytes{};
  std::size_t device_bytes{};
  if (!checked_multiply(
          identity.model.size(), sizeof(std::uint16_t), model_bytes) ||
      !checked_multiply(
          identity.device_instance_id.size(),
          sizeof(std::uint16_t),
          device_bytes)) {
    return clonecore::Result<std::size_t>::failure(admission_error(
        std::wstring(role) + L"識別情報長がオーバーフローします"));
  }
  std::size_t bytes = kIdentityCanonicalFixedBytes;
  if (!checked_add_bounded(
          bytes, model_bytes, kMaximumAdmissionCanonicalBytes) ||
      !checked_add_bounded(
          bytes,
          identity.serial_suffix.size(),
          kMaximumAdmissionCanonicalBytes) ||
      !checked_add_bounded(
          bytes, device_bytes, kMaximumAdmissionCanonicalBytes)) {
    return clonecore::Result<std::size_t>::failure(admission_error(
        std::wstring(role) + L"識別情報がcanonical byte上限を超えています"));
  }
  return clonecore::Result<std::size_t>::success(bytes);
}

clonecore::Result<operationcore::Sha256Digest> hash_backing_identity(
    const clonecore::StableDiskIdentity& identity) {
  try {
    auto identity_bytes = bounded_identity_canonical_size(
        identity, L"保存先backing");
    if (!identity_bytes) {
      return clonecore::Result<operationcore::Sha256Digest>::failure(
          identity_bytes.error());
    }
    std::size_t canonical_size = kBackingHashDomain.size();
    if (!checked_add_bounded(
            canonical_size,
            identity_bytes.value(),
            kMaximumAdmissionCanonicalBytes)) {
      return clonecore::Result<operationcore::Sha256Digest>::failure(
          admission_error(
              L"保存先backing識別Hashがcanonical byte上限を超えています"));
    }
    std::vector<std::byte> bytes;
    bytes.reserve(canonical_size);
    bytes.insert(
        bytes.end(), kBackingHashDomain.begin(), kBackingHashDomain.end());
    append_identity(bytes, identity);
    return imageformat::sha256(bytes);
  } catch (const std::bad_alloc&) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        product_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"Resume Slot data backing identity Hash",
            L"保存先識別Hash用メモリを確保できません"));
  } catch (const std::length_error&) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        product_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Resume Slot data backing identity Hash",
            L"保存先識別情報が安全上限を超えています"));
  }
}

clonecore::Status require_path_absent(
    const std::wstring& path,
    const std::wstring_view role) {
  SetLastError(ERROR_SUCCESS);
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_EXISTS,
        std::wstring(role),
        L"empty観測対象が存在します"));
  }
  const DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::query_failed,
        error,
        std::wstring(role),
        L"empty観測対象の不存在を証明できません"));
  }
  return clonecore::success_status();
}

clonecore::Result<operationcore::WindowsResumeDataBackingProof>
prove_current_data_backing(
    const std::span<const clonecore::StableDiskIdentity>
        protected_identities) {
  auto before = inspect_windows_startup_data_backing();
  if (!before) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        before.error());
  }
  auto provider = diskmodel::make_windows_disk_inventory_provider(nullptr);
  if (!provider) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        product_error(
            clonecore::ErrorCode::query_failed,
            ERROR_NOT_READY,
            L"Resume Slot data backing inventory",
            L"ディスク列挙providerを構成できません"));
  }
  auto inventory = provider->enumerate();
  if (!inventory) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        inventory.error());
  }
  auto after = inspect_windows_startup_data_backing();
  if (!after) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        after.error());
  }
  if (before.value().application_directory !=
          after.value().application_directory ||
      before.value().data_directory != after.value().data_directory ||
      before.value().disk_number != after.value().disk_number ||
      before.value().data_directory_exists !=
          after.value().data_directory_exists) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        product_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"Resume Slot data backing再観測",
            L"opened path観測中にEXE／data保存先が変化しました"));
  }

  const auto found = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [&](const diskmodel::DiskInfo& disk) {
        return disk.disk_number == before.value().disk_number;
      });
  if (found == inventory.value().disks.end()) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        product_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"Resume Slot data backing inventory",
            L"opened pathの物理ディスクを列挙結果で再識別できません"));
  }
  auto backing = diskmodel::make_stable_disk_identity(
      *found, found->is_system_disk);
  if (!backing) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        backing.error());
  }
  auto backing_hash = hash_backing_identity(backing.value());
  if (!backing_hash) {
    return clonecore::Result<
        operationcore::WindowsResumeDataBackingProof>::failure(
        backing_hash.error());
  }

  bool separated = !protected_identities.empty();
  for (const auto& expected : protected_identities) {
    bool current_identity_found = false;
    for (const auto& disk : inventory.value().disks) {
      auto current = diskmodel::make_stable_disk_identity(
          disk, disk.is_system_disk);
      if (!current || !identity_matches(expected, current.value())) {
        continue;
      }
      current_identity_found = true;
      if (identity_matches(current.value(), backing.value())) {
        separated = false;
      }
      break;
    }
    if (!current_identity_found) {
      separated = false;
    }
  }
  return clonecore::Result<
      operationcore::WindowsResumeDataBackingProof>::success({
      .backing_storage_identity_hash = backing_hash.take_value(),
      // inspect_windows_startup_data_backing maps the path through an opened
      // local-volume handle and we bind that disk to a fresh stable identity.
      .identity_from_open_handle = true,
      .separated_from_source = separated,
  });
}

class AbsentWindowsResumeSlotPlatform final
    : public operationcore::IResumeSlotPlatform {
 public:
  explicit AbsentWindowsResumeSlotPlatform(
      StartupDataBackingObservation reviewed)
      : reviewed_(std::move(reviewed)) {}

  clonecore::Result<operationcore::ResumeSlotObservation>
  observe_fixed_slot() override {
    try {
      return observe_absent_fixed_slot();
    } catch (const std::bad_alloc&) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(product_error(
          clonecore::ErrorCode::io_failed,
          ERROR_NOT_ENOUGH_MEMORY,
          L"Resume Slot absent data観測",
          L"empty観測用メモリを確保できません"));
    } catch (const std::length_error&) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(product_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot absent data観測",
          L"empty観測用path長が安全上限外です"));
    }
  }

 private:
  clonecore::Result<operationcore::ResumeSlotObservation>
  observe_absent_fixed_slot() {
    auto before = inspect_windows_startup_data_backing();
    if (!before) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(
          before.error());
    }
    const auto same = [&](const StartupDataBackingObservation& value) {
      return value.application_directory == reviewed_.application_directory &&
          value.data_directory == reviewed_.data_directory &&
          value.disk_number == reviewed_.disk_number &&
          !value.data_directory_exists;
    };
    if (!same(before.value())) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(
          product_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot absent data再観測",
              L"data不存在または親chainのopened backingがreview後に変化しました"));
    }
    std::filesystem::path checkpoint(reviewed_.data_directory);
    checkpoint /= operationcore::kResumeSlotFileName;
    std::filesystem::path stage(reviewed_.data_directory);
    stage /= L"active.checkpoint.new";
    const auto data_absent = require_path_absent(
        reviewed_.data_directory, L"Resume Slot data不存在");
    const auto checkpoint_absent = require_path_absent(
        checkpoint.native(), L"Resume Slot active.checkpoint不存在");
    const auto stage_absent = require_path_absent(
        stage.native(), L"Resume Slot stage object不存在");
    if (!data_absent || !checkpoint_absent || !stage_absent) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(
          !data_absent ? data_absent.error()
                       : !checkpoint_absent ? checkpoint_absent.error()
                                            : stage_absent.error());
    }
    auto after = inspect_windows_startup_data_backing();
    if (!after) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(after.error());
    }
    if (!same(after.value())) {
      return clonecore::Result<
          operationcore::ResumeSlotObservation>::failure(
          product_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Resume Slot absent data再観測",
              L"不存在object観測中にdataまたは親chainのopened backingが変化しました"));
    }
    return clonecore::Result<
        operationcore::ResumeSlotObservation>::success({
        .storage = {
            .checkpoint_path = checkpoint.native(),
            .paths_are_canonical_local = true,
            .parent_chain_reparse_free = true,
            // The data child is absent and mutations are disabled on this
            // platform.  This mirrors the production adapter's read-only
            // observe semantics; source separation is required only by its
            // create/replace entry points.
            .placement_separated_from_source = true,
            .checkpoint_and_partial_paths_distinct = true,
            .checkpoint_file = {},
            .owned_partial_file = {},
            .owned_object_files = {},
        },
        .slot = std::nullopt,
        .observed_owned_partial = std::nullopt,
        .observed_owned_objects = {},
    });
  }

 public:

  clonecore::Status create_fixed_slot(
      const operationcore::ResumeSlotRecord&) override {
    return mutation_forbidden();
  }

  clonecore::Status replace_fixed_slot(
      const operationcore::Sha256Digest&,
      const operationcore::ResumeSlotRecord&) override {
    return mutation_forbidden();
  }

  clonecore::Status discard_fixed_slot_and_owned_partial(
      const operationcore::ResumeSlotBinding&) override {
    return mutation_forbidden();
  }

 private:
  static clonecore::Status mutation_forbidden() {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"Resume Slot absent data platform",
        L"不存在dataの読取り専用empty観測では変更操作を許可しません"));
  }

  StartupDataBackingObservation reviewed_;
};

clonecore::Result<std::unique_ptr<operationcore::IResumeSlotPlatform>>
make_product_slot_platform(
    const std::span<const clonecore::StableDiskIdentity>
        protected_identities) {
  auto backing = inspect_windows_startup_data_backing();
  if (!backing) {
    return clonecore::Result<std::unique_ptr<
        operationcore::IResumeSlotPlatform>>::failure(backing.error());
  }
  if (!backing.value().data_directory_exists) {
    try {
      std::unique_ptr<operationcore::IResumeSlotPlatform> platform =
          std::make_unique<AbsentWindowsResumeSlotPlatform>(
              backing.take_value());
      return clonecore::Result<std::unique_ptr<
          operationcore::IResumeSlotPlatform>>::success(
          std::move(platform));
    } catch (const std::bad_alloc&) {
      return clonecore::Result<std::unique_ptr<
          operationcore::IResumeSlotPlatform>>::failure(product_error(
          clonecore::ErrorCode::io_failed,
          ERROR_NOT_ENOUGH_MEMORY,
          L"Resume Slot absent data platform",
          L"読取り専用platform用メモリを確保できません"));
    } catch (const std::length_error&) {
      return clonecore::Result<std::unique_ptr<
          operationcore::IResumeSlotPlatform>>::failure(product_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot absent data platform",
          L"読取り専用platform用文字列長が安全上限外です"));
    }
  }
  try {
    return operationcore::make_current_executable_windows_resume_slot_platform(
        [protected_identities =
             std::vector<clonecore::StableDiskIdentity>(
                 protected_identities.begin(), protected_identities.end())](
            const std::wstring&,
            const std::optional<operationcore::ResumeSlotRecord>&) {
          return prove_current_data_backing(protected_identities);
        });
  } catch (const std::bad_alloc&) {
    return clonecore::Result<std::unique_ptr<
        operationcore::IResumeSlotPlatform>>::failure(product_error(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Resume Slot production platform",
        L"保護対象identityの固定用メモリを確保できません"));
  } catch (const std::length_error&) {
    return clonecore::Result<std::unique_ptr<
        operationcore::IResumeSlotPlatform>>::failure(product_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot production platform",
        L"保護対象identityの件数が安全上限外です"));
  }
}

std::wstring capability_label(
    const operationcore::ResumeCapability capability) {
  switch (capability) {
    case operationcore::ResumeCapability::persistent_exact_restore:
      return L"通常イメージ復元";
    case operationcore::ResumeCapability::persistent_rescue_restore:
      return L"救出イメージ復元";
    case operationcore::ResumeCapability::persistent_pe_exact_image_create:
      return L"WinPE通常イメージ作成";
    case operationcore::ResumeCapability::same_process_only_vss_image_create:
    case operationcore::ResumeCapability::same_process_only_vss_clone:
    case operationcore::ResumeCapability::same_process_only_pe_image_create:
    case operationcore::ResumeCapability::same_process_only_pe_clone:
    case operationcore::ResumeCapability::unsupported_shrink_migration:
    case operationcore::ResumeCapability::unsupported_raw_rescue:
      return L"未対応の中断処理";
  }
  return L"未対応の中断処理";
}

std::wstring owned_discard_summary(
    const operationcore::ResumeSlotRecord& record) {
  std::wstring result =
      L"破棄対象（このcheckpointが完全拘束したアプリ所有物だけ）:\n"
      L"・EXE隣data\\active.checkpoint";
  if (record.owned_partial.has_value()) {
    result += L"\n・拘束済み .partial 出力 1件";
  }
  for (const auto& object : record.owned_objects) {
    result += L"\n・";
    switch (object.role) {
      case operationcore::ResumeOwnedObjectRole::image_partial:
        result += L"イメージ .partial";
        break;
      case operationcore::ResumeOwnedObjectRole::image_resume_journal:
        result += L"イメージ再開journal";
        break;
      case operationcore::ResumeOwnedObjectRole::rescue_stage:
        result += L"救出stage";
        break;
    }
  }
  result +=
      L"\n\n上記以外の既存ファイル、利用者ファイル、履歴は削除しません。";
  return result;
}

}  // namespace

namespace {

clonecore::Result<operationcore::OperationPlan>
make_windows_resume_slot_admission_plan_impl(
    const WindowsResumeSlotAdmissionReview& review) {
  const auto fields = review.immutable_review_fields;
  const auto digests = review.immutable_review_digests;
  if (fields.empty() && digests.empty() && !review.output_backing) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(
            L"実レビュー済みのoutput／操作固有条件が一つも拘束されていません"));
  }
  if (fields.size() > kMaximumAdmissionTextFields ||
      digests.size() > kMaximumAdmissionDigests) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビュー拘束フィールド数が安全上限を超えています"));
  }

  std::size_t canonical_size =
      kAdmissionHashDomain.size() + 1U + 1U + 4U + 4U;
  for (const auto field : fields) {
    if (field.empty()) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(L"空のレビュー拘束フィールドは使用できません"));
    }
    if (field.size() > kMaximumAdmissionFieldCharacters) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(
              L"レビュー拘束フィールドがUTF-16文字数上限を超えています"));
    }
    if (field.size() >
        (std::numeric_limits<std::size_t>::max)() /
            sizeof(std::uint16_t)) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(L"レビュー拘束フィールド長がオーバーフローします"));
    }
    const std::size_t field_bytes = field.size() * sizeof(std::uint16_t);
    if (canonical_size > kMaximumAdmissionCanonicalBytes - 8U ||
        field_bytes >
            kMaximumAdmissionCanonicalBytes - (canonical_size + 8U)) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(L"レビュー拘束のcanonical byte上限を超えています"));
    }
    canonical_size += 8U + field_bytes;
  }
  constexpr std::size_t kDigestBytes =
      std::tuple_size_v<operationcore::Sha256Digest>;
  if (digests.size() >
      (std::numeric_limits<std::size_t>::max)() / kDigestBytes) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビューHash拘束長がオーバーフローします"));
  }
  const std::size_t digest_bytes = digests.size() * kDigestBytes;
  if (digest_bytes > kMaximumAdmissionCanonicalBytes - canonical_size) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビューHash拘束のcanonical byte上限を超えています"));
  }
  canonical_size += digest_bytes;
  // StableDiskIdentity has the same bounded string shape enforced later by
  // validate_operation_plan.  Reserve a conservative upper bound before any
  // append; output identity is hashed only and is never assigned a false
  // OperationPlan source/target role.
  if (review.output_backing.has_value()) {
    const auto& output = review.output_backing.value();
    auto output_bytes = bounded_identity_canonical_size(
        output, L"output backing");
    if (!output_bytes) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          output_bytes.error());
    }
    if (!checked_add_bounded(
            canonical_size,
            output_bytes.value(),
            kMaximumAdmissionCanonicalBytes)) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          admission_error(L"output backing拘束がcanonical byte上限を超えています"));
    }
  }

  std::vector<std::byte> binding;
  try {
    binding.reserve(canonical_size);
    binding.insert(
        binding.end(),
        kAdmissionHashDomain.begin(),
        kAdmissionHashDomain.end());
    append_u8(binding, static_cast<std::uint8_t>(review.kind));
    append_u8(binding, review.output_backing.has_value() ? 1U : 0U);
    if (review.output_backing.has_value()) {
      append_identity(binding, review.output_backing.value());
    }
    append_u32(binding, static_cast<std::uint32_t>(fields.size()));
    for (const auto field : fields) {
      append_utf16(binding, field);
    }
    append_u32(binding, static_cast<std::uint32_t>(digests.size()));
    for (const auto& digest : digests) {
      binding.insert(binding.end(), digest.begin(), digest.end());
    }
  } catch (const std::bad_alloc&) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(
            L"レビュー拘束Hash用メモリを確保できません",
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY));
  } catch (const std::length_error&) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビュー拘束Hashの配列長が安全上限外です"));
  }
  if (binding.size() > kMaximumAdmissionCanonicalBytes) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"レビュー拘束のcanonical byte上限を超えています"));
  }
  auto payload_hash = imageformat::sha256(binding);
  if (!payload_hash) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        payload_hash.error());
  }
  operationcore::OperationPlan plan{
      .operation_id = review.operation_id,
      .kind = review.kind,
      .environment = operationcore::OperationEnvironment::windows,
      .source = review.source,
      .target = review.target,
      .expected_work_bytes = review.expected_work_bytes,
      .immutable_payload_hash = payload_hash.take_value(),
  };
  const auto valid = operationcore::validate_operation_plan(plan);
  if (!valid) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        valid.error());
  }
  return clonecore::Result<operationcore::OperationPlan>::success(
      std::move(plan));
}

}  // namespace

clonecore::Result<operationcore::OperationPlan>
make_windows_resume_slot_admission_plan(
    const WindowsResumeSlotAdmissionReview& review) {
  try {
    return make_windows_resume_slot_admission_plan_impl(review);
  } catch (const std::bad_alloc&) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(
            L"admission plan用メモリを確保できません",
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY));
  } catch (const std::length_error&) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        admission_error(L"admission planの配列長が安全上限外です"));
  }
}

clonecore::Result<operationcore::OperationId>
make_windows_resume_slot_admission_operation_id_with_windows_apis() {
  GUID guid{};
  const HRESULT status = CoCreateGuid(&guid);
  if (FAILED(status)) {
    return clonecore::Result<operationcore::OperationId>::failure(
        product_error(
            clonecore::ErrorCode::io_failed,
            static_cast<DWORD>(status),
            L"Windows SingleResumeSlot admission operation ID",
            L"単回admission IDを生成できません"));
  }
  operationcore::OperationId result{};
  static_assert(sizeof(guid) == result.size());
  std::memcpy(result.data(), &guid, result.size());
  return clonecore::Result<operationcore::OperationId>::success(result);
}

namespace {

clonecore::Result<WindowsResumeSlotStartupView>
make_windows_resume_slot_startup_view_impl(
    const std::optional<operationcore::ResumeSlotRecord>& record) {
  if (!record.has_value()) {
    return clonecore::Result<WindowsResumeSlotStartupView>::success({
        .active = false,
        .resume_action_available = false,
        .title = L"中断処理はありません",
        .details = L"固定SingleResumeSlotは空です。",
        .owned_discard_summary = {},
        .binding = std::nullopt,
    });
  }
  const auto valid = operationcore::validate_resume_slot_record(
      record.value());
  if (!valid) {
    return clonecore::Result<WindowsResumeSlotStartupView>::failure(
        valid.error());
  }
  auto binding = operationcore::make_resume_slot_binding(record.value());
  if (!binding) {
    return clonecore::Result<WindowsResumeSlotStartupView>::failure(
        binding.error());
  }
  const auto& checkpoint = record->checkpoint.checkpoint;
  std::wstring details =
      L"前回の中断処理: " + capability_label(record->capability) +
      L"\n検証済み進捗: " +
      std::to_wstring(checkpoint.verified_work_bytes) + L" / " +
      std::to_wstring(checkpoint.expected_work_bytes) + L" bytes" +
      L"\n\nこのWindows製品には、この永続checkpointを実行へ接続する"
      L"review済みresume backendがありません。再開操作は表示せず、"
      L"checkpointをそのまま残すか、完全bindingが再確認できたアプリ所有物だけを破棄できます。";
  return clonecore::Result<WindowsResumeSlotStartupView>::success({
      .active = true,
      .resume_action_available = false,
      .title = L"前回の中断処理を確認してください",
      .details = std::move(details),
      .owned_discard_summary = owned_discard_summary(record.value()),
      .binding = binding.take_value(),
  });
}

}  // namespace

clonecore::Result<WindowsResumeSlotStartupView>
make_windows_resume_slot_startup_view(
    const std::optional<operationcore::ResumeSlotRecord>& record) {
  try {
    return make_windows_resume_slot_startup_view_impl(record);
  } catch (const std::bad_alloc&) {
    return clonecore::Result<WindowsResumeSlotStartupView>::failure(
        product_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"Windows Resume Slot起動表示",
            L"起動表示用メモリを確保できません"));
  } catch (const std::length_error&) {
    return clonecore::Result<WindowsResumeSlotStartupView>::failure(
        product_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Windows Resume Slot起動表示",
            L"起動表示の文字列長が安全上限外です"));
  }
}

clonecore::Result<WindowsResumeSlotStartupView>
inspect_windows_resume_slot(operationcore::IResumeSlotPlatform& platform) {
  operationcore::SingleResumeSlot slot(platform);
  auto record = slot.inspect();
  if (!record) {
    return clonecore::Result<WindowsResumeSlotStartupView>::failure(
        record.error());
  }
  return make_windows_resume_slot_startup_view(record.value());
}

clonecore::Status guard_new_windows_operation_start(
    const operationcore::OperationPlan& admission_plan,
    operationcore::IResumeSlotPlatform& platform) {
  operationcore::SingleResumeSlot slot(platform);
  return slot.guard_new_operation_start(admission_plan);
}

clonecore::Status discard_bound_windows_resume_slot(
    const operationcore::ResumeSlotBinding& reviewed_binding,
    operationcore::IResumeSlotPlatform& platform) {
  operationcore::SingleResumeSlot slot(platform);
  // discard() first performs a fresh inspect/open_bound and the production
  // platform repeats the complete binding on opened objects before deletion.
  return slot.discard(reviewed_binding);
}

clonecore::Result<WindowsResumeSlotStartupView>
inspect_current_windows_resume_slot() {
  auto platform = make_product_slot_platform({});
  if (!platform) {
    return clonecore::Result<WindowsResumeSlotStartupView>::failure(
        platform.error());
  }
  return inspect_windows_resume_slot(*platform.value());
}

clonecore::Status guard_current_windows_operation_start(
    const operationcore::OperationPlan& admission_plan,
    const std::span<const clonecore::StableDiskIdentity>
        protected_identities) {
  auto platform = make_product_slot_platform(protected_identities);
  if (!platform) {
    return clonecore::Status::failure(platform.error());
  }
  return guard_new_windows_operation_start(
      admission_plan, *platform.value());
}

clonecore::Status discard_current_windows_resume_slot(
    const operationcore::ResumeSlotBinding& reviewed_binding) {
  auto platform = make_product_slot_platform({});
  if (!platform) {
    return clonecore::Status::failure(platform.error());
  }
  return discard_bound_windows_resume_slot(
      reviewed_binding, *platform.value());
}

}  // namespace ytec::windowsapp

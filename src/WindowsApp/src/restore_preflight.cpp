#include "ytec/windowsapp/restore_preflight.h"
#include "ytec/windowsapp/online_image_create.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <vector>

namespace ytec::windowsapp {
namespace {

clonecore::Error tsumugi_preflight_error(
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

bool selected_partition(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

clonecore::Result<std::wstring> canonical_tsumugi_path(
    const std::wstring& requested_path) {
  constexpr std::wstring_view extension = L".tsumugi";
  if (requested_path.size() <= extension.size() ||
      requested_path.size() >= 32767U ||
      requested_path.size() < 3U ||
      std::iswalpha(requested_path[0]) == 0 ||
      requested_path[1] != L':' ||
      (requested_path[2] != L'\\' && requested_path[2] != L'/') ||
      _wcsicmp(
          std::filesystem::path(requested_path).extension().c_str(),
          extension.data()) != 0) {
    return clonecore::Result<std::wstring>::failure(
        tsumugi_preflight_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"Tsumugi復元イメージパス",
            L"ローカルドライブ上の絶対.tsumugiパスを指定してください"));
  }
  const DWORD required =
      GetFullPathNameW(requested_path.c_str(), 0U, nullptr, nullptr);
  if (required == 0U || required >= 32767U) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"GetFullPathNameW(.tsumugi復元)",
            required == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::vector<wchar_t> buffer(required, L'\0');
  const DWORD written = GetFullPathNameW(
      requested_path.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (written == 0U || written >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"GetFullPathNameW(.tsumugi復元)",
            GetLastError()));
  }
  std::wstring canonical(buffer.data(), written);
  std::replace(canonical.begin(), canonical.end(), L'/', L'\\');
  return clonecore::Result<std::wstring>::success(std::move(canonical));
}

clonecore::Result<imageformat::TsumugiImageStorageFileSystem>
query_tsumugi_storage_file_system(const std::wstring& canonical_path) {
  std::array<wchar_t, MAX_PATH + 1U> root{};
  if (!GetVolumePathNameW(
          canonical_path.c_str(),
          root.data(),
          static_cast<DWORD>(root.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi復元元ボリューム取得",
            GetLastError()));
  }
  std::array<wchar_t, 32U> file_system{};
  if (!GetVolumeInformationW(
          root.data(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi復元元ファイルシステム取得",
            GetLastError()));
  }
  const auto type = _wcsicmp(file_system.data(), L"NTFS") == 0
      ? imageformat::TsumugiImageStorageFileSystem::ntfs
      : _wcsicmp(file_system.data(), L"exFAT") == 0
          ? imageformat::TsumugiImageStorageFileSystem::exfat
          : imageformat::TsumugiImageStorageFileSystem::other;
  return clonecore::Result<
      imageformat::TsumugiImageStorageFileSystem>::success(type);
}

clonecore::Result<std::uint64_t> minimum_shrink_target_bytes(
    const imageformat::TsumugiManifest& manifest) {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  constexpr std::uint64_t kFixedReserve = 64ULL * kMiB;
  std::uint64_t total = kFixedReserve;
  for (const auto& partition : manifest.partitions) {
    if (!selected_partition(partition)) {
      continue;
    }
    const std::uint64_t bytes =
        (std::max)(partition.minimum_target_bytes,
                   partition.planned_target_bytes);
    if (bytes == 0U ||
        bytes > (std::numeric_limits<std::uint64_t>::max)() - total - kMiB) {
      return clonecore::Result<std::uint64_t>::failure(
          tsumugi_preflight_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Tsumugi縮小復元容量計算",
              L"必要容量が空または安全に合計できません"));
    }
    total += bytes + kMiB;
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

}  // namespace

TsumugiRestorePasswordPromptDecision
evaluate_tsumugi_restore_password_prompt(
    const TsumugiRestorePasswordPromptState& state) noexcept {
  if (!state.header_probe_succeeded) {
    return TsumugiRestorePasswordPromptDecision::stop;
  }
  if (!state.encrypted) {
    return TsumugiRestorePasswordPromptDecision::no_password_required;
  }
  if (!state.prompt_accepted.has_value()) {
    return TsumugiRestorePasswordPromptDecision::prompt_required;
  }
  if (!state.prompt_accepted.value() || !state.password_available) {
    return TsumugiRestorePasswordPromptDecision::stop;
  }
  return TsumugiRestorePasswordPromptDecision::password_ready;
}

clonecore::Status validate_tsumugi_restore_storage_target_separation(
    const clonecore::StableDiskIdentity& image_storage,
    const clonecore::StableDiskIdentity& restore_target) {
  const auto storage_stable = clonecore::validate_stable_identity(
      image_storage, image_storage, L"復元イメージ保存ディスク");
  if (!storage_stable) {
    return storage_stable;
  }
  const auto target_stable = clonecore::validate_stable_identity(
      restore_target, restore_target, L"復元先ディスク");
  if (!target_stable) {
    return target_stable;
  }
  if (clonecore::validate_stable_identity(
          image_storage, restore_target, L"復元イメージ保存ディスク")) {
    return clonecore::Status::failure(tsumugi_preflight_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"復元イメージ保存ディスクと復元先の分離",
        L"復元イメージが保存されているディスクは復元先にできません"));
  }
  return clonecore::success_status();
}

clonecore::Result<TsumugiRestoreImagePreflightReport>
inspect_tsumugi_restore_image_file(
    const std::wstring& requested_path,
    const TsumugiRestoreImagePreflightOptions& options) {
  if (options.cancellation_requested &&
      options.cancellation_requested()) {
    return clonecore::Result<
        TsumugiRestoreImagePreflightReport>::failure(
        tsumugi_preflight_error(
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED,
            L"Tsumugi復元イメージ検証開始",
            L"開始前に安全に取り消されました"));
  }
  auto canonical = canonical_tsumugi_path(requested_path);
  if (!canonical) {
    return clonecore::Result<
        TsumugiRestoreImagePreflightReport>::failure(canonical.error());
  }
  auto storage = query_tsumugi_storage_file_system(canonical.value());
  if (!storage) {
    return clonecore::Result<
        TsumugiRestoreImagePreflightReport>::failure(storage.error());
  }
  auto verified = imageformat::verify_tsumugi_image_v1(
      imageformat::TsumugiImageVerifyRequest{
          .image_path = canonical.value(),
          .storage_file_system = storage.value(),
          .password = options.password,
      },
      clonecore::DiskOperationCallbacks{
          .progress = options.progress,
          .cancellation_requested = options.cancellation_requested,
      });
  if (!verified) {
    return clonecore::Result<
        TsumugiRestoreImagePreflightReport>::failure(verified.error());
  }
  const auto& header = verified.value().container.header;
  if (header.footer.length != imageformat::kTsumugiFooterSize ||
      header.footer.offset >
          (std::numeric_limits<std::uint64_t>::max)() -
              header.footer.length) {
    return clonecore::Result<
        TsumugiRestoreImagePreflightReport>::failure(
        tsumugi_preflight_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Tsumugi復元イメージ全長",
            L"検証済みフッターから安全に全長を計算できません"));
  }
  const bool encrypted =
      (header.required_features &
       static_cast<std::uint32_t>(
           imageformat::TsumugiRequiredFeature::encrypted)) != 0U;
  return clonecore::Result<
      TsumugiRestoreImagePreflightReport>::success(
      TsumugiRestoreImagePreflightReport{
          .canonical_path = canonical.take_value(),
          .storage_file_system = storage.value(),
          .image_length = header.footer.offset + header.footer.length,
          .global_hash = verified.value().container.global_hash,
          .header = verified.value().container.header,
          .manifest = std::move(verified.value().manifest),
          .unreadable_ranges =
              std::move(verified.value().unreadable_ranges),
          .encrypted = encrypted,
          .partial_loss = verified.value().partial_loss,
          .complete_container_verified = true,
          .metadata_verified = true,
          .restore_layout_verified = true,
          .restore_execution_enabled = false,
      });
}

RestoreTargetSelectionView evaluate_tsumugi_restore_target_selection(
    const TsumugiRestoreImagePreflightReport* const image,
    const diskmodel::InventoryReport* const inventory,
    const std::optional<std::size_t> target_index,
    const bool inventory_loading,
    const std::optional<imageformat::
        TsumugiPhysicalIndividualPartitionRestoreSelection>
        individual_partition) {
  if (image == nullptr || !image->complete_container_verified ||
      !image->metadata_verified || !image->restore_layout_verified) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::image_unavailable,
        .message =
            L"先に.tsumugiイメージの読み取り専用完全検証を完了してください。"};
  }
  if (inventory_loading) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::inventory_loading,
        .message = L"復元先候補を読み取り専用で確認しています…"};
  }
  if (inventory == nullptr || inventory->disks.empty()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::inventory_unavailable,
        .message = L"復元先候補のディスク情報を取得できません。"};
  }
  if (!inventory->issues.empty()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::inventory_has_issues,
        .message =
            L"未解決のディスク列挙診断があるため候補を確定できません。"};
  }
  if (!target_index.has_value() ||
      target_index.value() >= inventory->disks.size()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_not_selected,
        .message = L"読み取り専用で確認する復元先候補を選択してください。"};
  }

  const auto& target = inventory->disks[target_index.value()];
  auto identity = diskmodel::make_stable_disk_identity(
      target, target.is_system_disk);
  if (!identity) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::unstable_identity,
        .message = L"候補の安定識別情報が不足しているため進めません。"};
  }
  const auto model_hash = hash_tsumugi_source_model(target.model);
  const auto serial_hash = hash_tsumugi_source_serial(
      target.serial_suffix, target.device_instance_id);
  if (!model_hash || !serial_hash) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::unstable_identity,
        .message =
            L"候補のモデル／シリアル識別Hashを安全に作成できません。"};
  }
  if (model_hash.value() == image->manifest.source_model_hash &&
      serial_hash.value() == image->manifest.source_serial_hash) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_original_source,
        .message =
            L"イメージに記録されたコピー元と同じディスクは復元先にできません。"};
  }
  if (target.is_system_disk) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_system,
        .message =
            L"現在動作中のWindowsディスクはWindows上から復元できません。PEで改めて選択してください。"};
  }
  if (!target.read_only.has_value() || !target.removable.has_value()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_state_unknown,
        .message =
            L"読取り専用またはリムーバブル状態が不明なため進めません。"};
  }
  if (target.read_only.value()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_read_only,
        .message = L"候補ディスクが読み取り専用です。"};
  }
  if (target.removable.value()) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_is_removable,
        .message =
            L"USBメモリ等のリムーバブル媒体は復元先にできません。"};
  }
  if (diskmodel::disk_health_operation_advice(target.health, false) ==
      diskmodel::DiskHealthOperationAdvice::block_target) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_health_abnormal,
        .message =
            L"復元先のSMART／NVMe健康状態が注意または異常のため開始できません。"};
  }
  if (target.partition_style == diskmodel::PartitionStyle::unknown) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_style_unknown,
        .message =
            L"候補ディスクのパーティション形式を安全に確認できません。"};
  }
  if (target.logical_sector_size != image->manifest.logical_sector_size) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::logical_sector_mismatch,
        .message =
            L"現在の復元経路では論理セクターサイズが一致する対象だけを選択できます。"};
  }

  const bool shrink =
      image->manifest.mode == imageformat::TsumugiManifestMode::shrink;
  if (individual_partition.has_value()) {
    if (shrink) {
      return RestoreTargetSelectionView{
          .issue =
              RestoreTargetSelectionIssue::individual_selection_invalid,
          .message =
              L"縮小イメージはディスク全体のレビュー済み配置だけに復元できます。"};
    }
    const auto individual_status = imageformat::
        validate_tsumugi_physical_individual_partition_selection_v1(
            image->manifest, target, individual_partition.value());
    if (!individual_status) {
      return RestoreTargetSelectionView{
          .issue =
              RestoreTargetSelectionIssue::individual_selection_invalid,
          .message = individual_status.error().message};
    }
    const bool unallocated = std::holds_alternative<imageformat::
        TsumugiPhysicalUnallocatedRestoreSelection>(
        individual_partition->target);
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::ready_for_confirmation,
        .ready_for_confirmation = true,
        .restore_execution_enabled = false,
        .target_identity = identity.take_value(),
        .message = unallocated
            ? L"未割当範囲・同一GPT/MBR形式・空きentry・必要容量・セクターは確認済みです。既存区画表を保持し、新規1区画だけをデータ検証後に確定します。"
            : L"既存パーティションの番号・範囲・必要容量・セクターは確認済みです。実行直前の再識別とOK確認はまだ行っていません。"};
  }

  std::uint64_t required_bytes = image->manifest.source_disk_size;
  if (shrink) {
    const auto minimum = minimum_shrink_target_bytes(image->manifest);
    if (!minimum) {
      return RestoreTargetSelectionView{
          .issue = RestoreTargetSelectionIssue::target_too_small,
          .message = minimum.error().message};
    }
    required_bytes = minimum.value();
  }
  if (target.size_bytes < required_bytes) {
    return RestoreTargetSelectionView{
        .issue = RestoreTargetSelectionIssue::target_too_small,
        .message = shrink
            ? L"縮小後の各領域、安全余白、パーティション境界を配置できません。"
            : L"候補の容量がイメージ元ディスクより小さいため進めません。"};
  }

  return RestoreTargetSelectionView{
      .issue = RestoreTargetSelectionIssue::ready_for_confirmation,
      .ready_for_confirmation = true,
      .restore_execution_enabled = false,
      .target_identity = identity.take_value(),
      .message =
          std::wstring(shrink
              ? L"縮小復元の概算必要容量・セクター・基本状態は確認済みです。"
              : L"容量・セクター・基本状態は確認済みです。") +
          L"実行直前の再識別、詳細レイアウト計画、OK確認はまだ行っていません。"};
}

}  // namespace ytec::windowsapp

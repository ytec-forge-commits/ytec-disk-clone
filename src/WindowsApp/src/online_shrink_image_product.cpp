#include "ytec/windowsapp/online_shrink_image_product.h"

#include "ytec/imageformat/windows_tsumugi_destination.h"
#include "ytec/windowsapp/online_image_create.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::uint64_t kContainerReserveBytes = 64ULL * 1024ULL * 1024ULL;

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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(product_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool drive_absolute(const std::wstring_view value) noexcept {
  return value.size() >= 3U &&
      ((value[0] >= L'A' && value[0] <= L'Z') ||
       (value[0] >= L'a' && value[0] <= L'z')) &&
      value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

clonecore::Result<std::wstring> canonical_local_path(
    const std::wstring& requested,
    const std::wstring_view operation) {
  if (!drive_absolute(requested) ||
      requested.size() >= kMaximumPathCharacters) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ローカルドライブ上の絶対パスが必要です");
  }
  std::vector<wchar_t> buffer(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFullPathNameW(
      requested.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_argument,
            std::wstring(operation),
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::wstring canonical(buffer.data(), length);
  std::replace(canonical.begin(), canonical.end(), L'/', L'\\');
  if (!drive_absolute(canonical)) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"正規化後のパスがローカルドライブを指していません");
  }
  return clonecore::Result<std::wstring>::success(std::move(canonical));
}

clonecore::Status verify_directory_chain(
    const std::filesystem::path& directory,
    const std::wstring_view operation) {
  if (!directory.is_absolute() || directory.root_name().wstring().size() != 2U) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"作業パスの親ディレクトリが不正です"));
  }
  std::filesystem::path current = directory.root_path();
  const auto check = [&](const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Status::failure(product_error(
          clonecore::ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                 : ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"存在する通常ディレクトリだけを経由できます"));
    }
    return clonecore::success_status();
  };
  auto status = check(current);
  if (!status) {
    return status;
  }
  for (const auto& component : directory.relative_path()) {
    if (component == L".") {
      continue;
    }
    if (component == L"..") {
      return clonecore::Status::failure(product_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          std::wstring(operation),
          L"親参照を含む作業パスは使用できません"));
    }
    current /= component;
    status = check(current);
    if (!status) {
      return status;
    }
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint32_t> observe_path_disk_number(
    const std::wstring& canonical,
    const bool directory,
    const std::wstring_view operation) {
  const std::filesystem::path path(canonical);
  const std::filesystem::path parent = directory ? path : path.parent_path();
  auto status = verify_directory_chain(parent, operation);
  if (!status) {
    return clonecore::Result<std::uint32_t>::failure(status.error());
  }
  if (directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return failure<std::uint32_t>(
          clonecore::ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                 : ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"scratchは存在する通常ディレクトリである必要があります");
    }
  } else {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)) {
      return failure<std::uint32_t>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"作業ファイル位置が通常ファイルではありません");
    }
  }
  const std::wstring probe = directory
      ? (path / L".ytec-shrink-placement-probe").wstring()
      : canonical;
  return diskmodel::query_single_disk_number_for_local_path(probe);
}

[[maybe_unused]] clonecore::Result<WindowsShrinkWorkPlacementObservation>
observe_work_placement_with_windows_apis(
    const WindowsShrinkWorkPaths& paths) {
  auto scratch = canonical_local_path(
      paths.scratch_directory, L"縮小移行scratch正規化");
  auto checkpoint = canonical_local_path(
      paths.checkpoint_path, L"縮小移行checkpoint正規化");
  if (!scratch || !checkpoint) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        scratch ? checkpoint.error() : scratch.error());
  }
  clonecore::Result<std::wstring> log =
      paths.log_is_ram_only
      ? clonecore::Result<std::wstring>::success({})
      : canonical_local_path(paths.log_path, L"縮小移行log正規化");
  if (!log || (paths.log_is_ram_only && !paths.log_path.empty())) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        log ? product_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"縮小移行RAM log",
                  L"RAM限定ログにfilesystem pathを指定できません")
            : log.error());
  }

  auto scratch_before = observe_path_disk_number(
      scratch.value(), true, L"縮小移行scratch物理ディスク");
  auto checkpoint_before = observe_path_disk_number(
      checkpoint.value(), false, L"縮小移行checkpoint物理ディスク");
  clonecore::Result<std::uint32_t> log_before =
      paths.log_is_ram_only
      ? clonecore::Result<std::uint32_t>::success(0U)
      : observe_path_disk_number(
            log.value(), false, L"縮小移行log物理ディスク");
  if (!scratch_before || !checkpoint_before || !log_before) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        !scratch_before
            ? scratch_before.error()
            : !checkpoint_before ? checkpoint_before.error()
                                 : log_before.error());
  }

  auto provider = diskmodel::make_windows_disk_inventory_provider();
  auto inventory = provider->enumerate();
  if (!inventory) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        inventory.error());
  }
  auto scratch_after = observe_path_disk_number(
      scratch.value(), true, L"縮小移行scratch物理ディスク再確認");
  auto checkpoint_after = observe_path_disk_number(
      checkpoint.value(), false, L"縮小移行checkpoint物理ディスク再確認");
  clonecore::Result<std::uint32_t> log_after =
      paths.log_is_ram_only
      ? clonecore::Result<std::uint32_t>::success(0U)
      : observe_path_disk_number(
            log.value(), false, L"縮小移行log物理ディスク再確認");
  if (!scratch_after || !checkpoint_after || !log_after ||
      scratch_before.value() != scratch_after.value() ||
      checkpoint_before.value() != checkpoint_after.value() ||
      log_before.value() != log_after.value()) {
    return failure<WindowsShrinkWorkPlacementObservation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"縮小移行作業場所再識別",
        L"ディスク列挙の前後で作業場所の物理ディスクが変化しました");
  }

  const auto identity_for = [&](const std::uint32_t disk_number)
      -> clonecore::Result<clonecore::StableDiskIdentity> {
    const auto found = std::find_if(
        inventory.value().disks.begin(),
        inventory.value().disks.end(),
        [disk_number](const auto& disk) {
          return disk.disk_number == disk_number;
        });
    if (found == inventory.value().disks.end()) {
      return failure<clonecore::StableDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"縮小移行作業場所安定識別",
          L"作業場所の物理ディスクを現在の列挙へ対応付けできません");
    }
    return diskmodel::make_stable_disk_identity(*found, found->is_system_disk);
  };
  auto scratch_identity = identity_for(scratch_before.value());
  auto checkpoint_identity = identity_for(checkpoint_before.value());
  clonecore::Result<clonecore::StableDiskIdentity> log_identity =
      paths.log_is_ram_only
      ? clonecore::Result<clonecore::StableDiskIdentity>::success({})
      : identity_for(log_before.value());
  if (!scratch_identity || !checkpoint_identity || !log_identity) {
    return clonecore::Result<WindowsShrinkWorkPlacementObservation>::failure(
        !scratch_identity
            ? scratch_identity.error()
            : !checkpoint_identity ? checkpoint_identity.error()
                                   : log_identity.error());
  }
  return clonecore::Result<WindowsShrinkWorkPlacementObservation>::success({
      .scratch = {
          .canonical_path = scratch.take_value(),
          .backing_disk = scratch_identity.take_value(),
          .local_volume = true,
      },
      .checkpoint = {
          .canonical_path = checkpoint.take_value(),
          .backing_disk = checkpoint_identity.take_value(),
          .local_volume = true,
      },
      .log = paths.log_is_ram_only
          ? WindowsShrinkWorkPathObservation{}
          : WindowsShrinkWorkPathObservation{
                .canonical_path = log.take_value(),
                .backing_disk = log_identity.take_value(),
                .local_volume = true,
            },
  });
}

clonecore::Result<imageformat::TsumugiImageStorageFileSystem>
query_destination_file_system(const std::wstring& final_path) {
  const std::filesystem::path parent = std::filesystem::path(final_path).parent_path();
  if (parent.empty()) {
    return failure<imageformat::TsumugiImageStorageFileSystem>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"縮小Tsumugi保存先filesystem",
        L"保存先の親ディレクトリを特定できません");
  }
  std::vector<wchar_t> root(kMaximumPathCharacters, L'\0');
  if (!GetVolumePathNameW(
          parent.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小Tsumugi保存先Volume root",
            GetLastError()));
  }
  std::vector<wchar_t> file_system(64U, L'\0');
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
            L"縮小Tsumugi保存先filesystem照会",
            GetLastError()));
  }
  if (_wcsicmp(file_system.data(), L"NTFS") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::ntfs);
  }
  if (_wcsicmp(file_system.data(), L"exFAT") == 0) {
    return clonecore::Result<
        imageformat::TsumugiImageStorageFileSystem>::success(
        imageformat::TsumugiImageStorageFileSystem::exfat);
  }
  return failure<imageformat::TsumugiImageStorageFileSystem>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"縮小Tsumugi保存先filesystem",
      L"単一.tsumugiの保存先はNTFSまたはexFATに限ります");
}

clonecore::Result<std::uint64_t> required_destination_bytes(
    const imageformat::TsumugiManifest& manifest) {
  std::uint64_t total{};
  for (const auto& partition : manifest.partitions) {
    if (!checked_add(total, partition.planned_target_bytes, total)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小Tsumugi保存先容量上限",
          L"パーティション容量合計が64bit上限を超えます");
    }
  }
  std::uint64_t required{};
  if (!checked_add(total, kContainerReserveBytes, required)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小Tsumugiコンテナ容量上限",
        L"安全余白を含む保存先容量が64bit上限を超えます");
  }
  return clonecore::Result<std::uint64_t>::success(required);
}

clonecore::Result<windowsshrink::ShrinkSourceAnalysis> analyze_source(
    const WindowsOnlineShrinkImageProductRequest& request,
    const clonecore::StableDiskIdentity& identity) {
  auto opened =
      diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          identity);
  if (!opened) {
    return clonecore::Result<windowsshrink::ShrinkSourceAnalysis>::failure(
        opened.error());
  }
  windowsshrink::ShrinkSourceAnalysisContext context{
      .source_identity = opened.value().observed.identity,
      .physical_sector_size =
          opened.value().observed.observed.physical_sector_size,
      .created_utc = request.created_utc,
      .app_version = request.app_version,
  };
  if (identity.is_system_disk) {
    context.known_windows_version = windowsshrink::WindowsSourceVersion{
        .major = request.windows_major,
        .minor = request.windows_minor,
        .build = request.windows_build,
        .architecture = request.windows_architecture,
    };
  }
  if (opened.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::gpt) {
    auto layout = clonecore::parse_gpt(*opened.value().reader);
    if (!layout) {
      return clonecore::Result<windowsshrink::ShrinkSourceAnalysis>::failure(
          layout.error());
    }
    return windowsshrink::analyze_gpt_shrink_source_with_windows_apis(
        opened.value().observed.observed,
        *opened.value().reader,
        layout.value(),
        context);
  }
  if (opened.value().observed.observed.partition_style ==
      diskmodel::PartitionStyle::mbr) {
    auto layout = clonecore::parse_mbr(*opened.value().reader);
    if (!layout) {
      return clonecore::Result<windowsshrink::ShrinkSourceAnalysis>::failure(
          layout.error());
    }
    return windowsshrink::analyze_mbr_shrink_source_with_windows_apis(
        opened.value().observed.observed,
        *opened.value().reader,
        layout.value(),
        context);
  }
  return failure<windowsshrink::ShrinkSourceAnalysis>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"Windows縮小Tsumugi partition style",
      L"GPTまたはMBRの基本ディスクだけを解析できます");
}

}  // namespace

clonecore::Result<vssrequester::OnlineTsumugiBackupReport>
execute_windows_online_shrink_image_create_with_windows_apis(
    const WindowsOnlineShrinkImageProductRequest& request) {
  auto identity = diskmodel::make_stable_disk_identity(
      request.selected_source, request.selected_source.is_system_disk);
  if (!identity) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(identity.error());
  }
  if (!request.administrator || request.final_path.empty() ||
      request.log_is_ram_only != request.persistent_log_path.empty() ||
      !request.diff_area_review_callback) {
    return failure<vssrequester::OnlineTsumugiBackupReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows縮小Tsumugi製品要求",
        L"管理者、保存先、RAM／永続ログ指定、またはVSS差分領域review UIが不正です");
  }

  imageformat::WindowsTsumugiDestinationGuardRequest destination_guard{
      .final_path = request.final_path,
      .expected_source_disk = identity.value(),
      .required_available_bytes = 1U,
      .replace_existing = request.replace_existing,
      .phase = imageformat::
          WindowsTsumugiDestinationGuardPhase::before_stage,
  };
  auto status = imageformat::validate_windows_tsumugi_destination(
      destination_guard);
  if (!status) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(status.error());
  }
  auto storage = query_destination_file_system(request.final_path);
  if (!storage) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(storage.error());
  }
  auto analysis = analyze_source(request, identity.value());
  if (!analysis) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(analysis.error());
  }
  auto source_hashes = make_tsumugi_source_identity_hashes(
      analysis.value().source,
      analysis.value().physical_sector_size,
      analysis.value().partition_snapshot);
  if (!source_hashes) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        source_hashes.error());
  }
  auto source_token =
      vssrequester::encode_vss_diff_area_source_epoch_token(
          std::span<const std::byte>(source_hashes.value().locked_state));
  if (!source_token) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(
        source_token.error());
  }
  const auto expected_source_identity = identity.value();
  const std::wstring expected_source_token = source_token.take_value();
  auto plan = plan_windows_online_shrink_image(
      WindowsOnlineShrinkImagePlanRequest{
          .analysis = analysis.take_value(),
          .final_path = request.final_path,
          .storage_file_system = storage.value(),
          .administrator = request.administrator,
          .encryption_password = request.encryption_password,
          .verification_mode = request.verification_mode,
          .replace_existing = request.replace_existing,
      });
  if (!plan) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(plan.error());
  }
  auto required = required_destination_bytes(plan.value().image_template.manifest);
  if (!required) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(required.error());
  }
  destination_guard.required_available_bytes = required.value();
  status = imageformat::validate_windows_tsumugi_destination(
      destination_guard);
  if (!status) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(status.error());
  }

  const std::filesystem::path final_path(request.final_path);
  WindowsShrinkWorkPaths work_paths{
      .scratch_directory = final_path.parent_path().wstring(),
      .checkpoint_path = request.final_path + L".checkpoint",
      .log_path = request.persistent_log_path,
      .log_is_ram_only = request.log_is_ram_only,
  };
  auto capture =
      make_windows_ntfs_shrink_capture_executor_with_windows_apis(
          plan.value().capture);
  if (!capture) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(capture.error());
  }
  const auto guard = destination_guard;
  auto execution = execute_windows_online_shrink_image_create(
      WindowsOnlineShrinkImageCreateRequest{
          .source_disk = identity.take_value(),
          .workflow = std::move(plan.value().workflow),
          .image_template = std::move(plan.value().image_template),
          .work_paths = std::move(work_paths),
          .callbacks = request.callbacks,
      },
      WindowsOnlineShrinkImageCreateDependencies{
          .observe_work_placement =
              observe_windows_shrink_work_placement_with_windows_apis,
          .capture_snapshot_payloads = capture.take_value(),
          .prepare_image =
              [](const imageformat::TsumugiImageCreateRequest& image,
                 const clonecore::DiskOperationCallbacks& callbacks) {
                return imageformat::prepare_tsumugi_image_v1(
                    image, callbacks);
              },
          .backend_factory =
              [async_wait = request.async_wait,
               logger = request.logger](
                  vssrequester::SnapshotCopyCallback callback) {
                std::unique_ptr<vssrequester::IWorkflowBackend> backend =
                    std::make_unique<vssrequester::WindowsVssBackend>(
                        vssrequester::WindowsVssBackendOptions{
                            .async_wait = async_wait,
                            .copy_snapshot_data = std::move(callback),
                            .logger = logger,
                        });
                return clonecore::Result<std::unique_ptr<
                    vssrequester::IWorkflowBackend>>::success(
                    std::move(backend));
              },
          .revalidate_destination =
              [guard](const imageformat::TsumugiImageCreateReport* report) mutable {
                auto current = guard;
                if (report != nullptr) {
                  current.phase = imageformat::
                      WindowsTsumugiDestinationGuardPhase::
                          before_commit_owned_partial;
                  current.expected_owned_partial_bytes =
                      report->stream.image_length;
                }
                return imageformat::validate_windows_tsumugi_destination(
                    current);
              },
          .make_diff_area_monitor =
              [request,
               expected_source_identity,
               expected_source_token](
                  const vssrequester::SnapshotCopyContext& context) {
                return vssrequester::
                    make_windows_vss_diff_area_operation_monitor(
                        context,
                        vssrequester::
                            WindowsVssDiffAreaOperationMonitorOptions{
                            .expected_source_identity_token =
                                expected_source_token,
                            .probe_source_identity =
                                [request,
                                 expected_source_identity](
                                    const vssrequester::
                                        VssDiffAreaSnapshotBinding&) {
                                  auto observed = analyze_source(
                                      request,
                                      expected_source_identity);
                                  if (!observed) {
                                    return clonecore::Result<
                                        std::wstring>::failure(
                                        observed.error());
                                  }
                                  auto hashes =
                                      make_tsumugi_source_identity_hashes(
                                          observed.value().source,
                                          observed.value()
                                              .physical_sector_size,
                                          observed.value()
                                              .partition_snapshot);
                                  if (!hashes) {
                                    return clonecore::Result<
                                        std::wstring>::failure(
                                        hashes.error());
                                  }
                                  return vssrequester::
                                      encode_vss_diff_area_source_epoch_token(
                                          std::span<const std::byte>(
                                              hashes.value().locked_state));
                                },
                            .review_callback =
                                request.diff_area_review_callback,
                            .logger = request.logger,
                        });
              },
      });
  if (!execution) {
    return clonecore::Result<
        vssrequester::OnlineTsumugiBackupReport>::failure(execution.error());
  }
  if (request.logger != nullptr) {
    request.logger->info(
        L"Windows縮小.tsumugi作成完了 wim_count=" +
        std::to_wstring(execution.value().wim_payload_count) +
        L" exact_raw_count=" +
        std::to_wstring(execution.value().exact_raw_payload_count));
  }
  return clonecore::Result<vssrequester::OnlineTsumugiBackupReport>::success({
      .workflow = execution.value().workflow,
      .image = execution.value().image,
      .final_file_committed_after_vss =
          execution.value().final_file_committed_after_vss,
  });
}

}  // namespace ytec::windowsapp

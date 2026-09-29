#include "ytec/migrationengine/bundle_capture.h"

#include "ytec/imageformat/shrink_image_manifest.h"
#include "ytec/migrationengine/shrink_bundle.h"
#include "ytec/windowsdism/dism.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

constexpr std::uint32_t kDismTimeoutMilliseconds = 6U * 60U * 60U * 1000U;

clonecore::Error capture_error(
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
  return clonecore::Result<T>::failure(capture_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool same_path_case_insensitive(
    const std::wstring& left,
    const std::wstring& right) noexcept {
  return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring normalize_capture_root(std::wstring root) {
  if (!root.empty() && !root.ends_with(L'\\')) {
    root.push_back(L'\\');
  }
  return root;
}

clonecore::Status validate_request(const ShrinkBundleCaptureRequest& request) {
  const std::filesystem::path final_path(request.final_bundle_directory);
  const std::filesystem::path scratch(request.scratch_directory);
  if (!final_path.is_absolute() ||
      _wcsicmp(final_path.extension().c_str(), L".dcmig") != 0 ||
      final_path.filename() == L".dcmig" || !scratch.is_absolute() ||
      request.analysis.content_volumes.empty() ||
      request.capture_sources.size() != request.analysis.content_volumes.size()) {
    return clonecore::Status::failure(capture_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"縮小イメージ作成要求",
        L"保存先、作業フォルダー、または対象ボリューム件数が不正です"));
  }
  if (GetFileAttributesW(request.final_bundle_directory.c_str()) !=
      INVALID_FILE_ATTRIBUTES) {
    return clonecore::Status::failure(capture_error(
        clonecore::ErrorCode::access_denied,
        ERROR_FILE_EXISTS,
        L"縮小イメージ保存先",
        L"既存のファイルまたはフォルダーは上書きしません"));
  }
  const DWORD scratch_attributes =
      GetFileAttributesW(request.scratch_directory.c_str());
  if (scratch_attributes == INVALID_FILE_ATTRIBUTES ||
      (scratch_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (scratch_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Status::failure(capture_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_PATH_NOT_FOUND,
        L"縮小イメージ作業フォルダー",
        L"通常フォルダーの絶対パスを指定してください"));
  }
  for (std::size_t index = 0; index < request.capture_sources.size(); ++index) {
    const auto& supplied = request.capture_sources[index];
    const auto expected = std::find_if(
        request.analysis.content_volumes.begin(),
        request.analysis.content_volumes.end(),
        [&](const auto& volume) {
          return volume.source_table_index == supplied.source_table_index;
        });
    if (expected == request.analysis.content_volumes.end() ||
        supplied.capture_root.empty()) {
      return clonecore::Status::failure(capture_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"縮小イメージ対象ボリューム",
          L"解析済みパーティションとキャプチャ元が一致しません"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (request.capture_sources[previous].source_table_index ==
              supplied.source_table_index ||
          same_path_case_insensitive(
              request.capture_sources[previous].capture_root,
              supplied.capture_root)) {
        return clonecore::Status::failure(capture_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"縮小イメージ対象ボリューム",
            L"パーティション番号またはキャプチャ元が重複しています"));
      }
    }
  }
  return clonecore::success_status();
}

std::wstring staging_path_for(const std::wstring& final_path) {
  const std::filesystem::path path(final_path);
  const std::wstring name = path.stem().wstring() + L".partial-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(GetTickCount64()) + L".dcmig";
  return (path.parent_path() / name).wstring();
}

void cleanup_known_staging(
    const std::wstring& directory,
    const std::vector<std::wstring>& created_files) noexcept {
  for (auto file = created_files.rbegin(); file != created_files.rend(); ++file) {
    (void)DeleteFileW(file->c_str());
  }
  (void)RemoveDirectoryW(directory.c_str());
}

clonecore::Status write_new_file(
    const std::wstring& path,
    const std::vector<std::byte>& bytes) {
  if (bytes.empty() ||
      bytes.size() > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
    return clonecore::Status::failure(capture_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILE_TOO_LARGE,
        L"縮小イメージマニフェスト保存",
        L"マニフェスト長が不正です"));
  }
  HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"縮小イメージマニフェスト新規作成",
        GetLastError()));
  }
  DWORD written = 0;
  const bool complete = WriteFile(
      file,
      bytes.data(),
      static_cast<DWORD>(bytes.size()),
      &written,
      nullptr) &&
      written == bytes.size() && FlushFileBuffers(file);
  const DWORD native_code = complete ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!complete) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"縮小イメージマニフェスト保存",
        native_code == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native_code));
  }
  return clonecore::success_status();
}

std::optional<std::wstring> query_system_directory() {
  std::vector<wchar_t> path(32U * 1024U, L'\0');
  const UINT length = GetSystemDirectoryW(
      path.data(), static_cast<UINT>(path.size()));
  if (length == 0U || length >= path.size()) {
    return std::nullopt;
  }
  return std::wstring(path.data(), length);
}

}  // namespace

clonecore::Result<ShrinkBundleCaptureReport> capture_shrink_bundle(
    const ShrinkBundleCaptureRequest& request,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner) {
  const auto valid = validate_request(request);
  if (!valid) {
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(valid.error());
  }
  if (request.callbacks.cancellation_requested &&
      request.callbacks.cancellation_requested()) {
    return failure<ShrinkBundleCaptureReport>(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"縮小イメージ作成開始",
        L"開始前に安全に取り消されました");
  }
  const std::wstring staging = staging_path_for(request.final_bundle_directory);
  if (!CreateDirectoryW(staging.c_str(), nullptr)) {
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"縮小イメージ一時フォルダー作成",
            GetLastError()));
  }
  std::vector<std::wstring> created_files;
  auto manifest = request.analysis.manifest;
  std::uint64_t total_payload = 0U;
  for (const auto& source : request.capture_sources) {
    if (request.callbacks.cancellation_requested &&
        request.callbacks.cancellation_requested()) {
      cleanup_known_staging(staging, created_files);
      return failure<ShrinkBundleCaptureReport>(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"縮小イメージ作成",
          L"ボリューム間の安全な境界で取り消しました");
    }
    const auto analyzed = std::find_if(
        request.analysis.content_volumes.begin(),
        request.analysis.content_volumes.end(),
        [&](const auto& value) {
          return value.source_table_index == source.source_table_index;
        });
    const auto partition = std::find_if(
        manifest.partitions.begin(),
        manifest.partitions.end(),
        [&](const auto& value) {
          return value.source_table_index == source.source_table_index;
        });
    if (analyzed == request.analysis.content_volumes.end() ||
        partition == manifest.partitions.end()) {
      cleanup_known_staging(staging, created_files);
      return failure<ShrinkBundleCaptureReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"縮小イメージ解析結果接続",
          L"解析済みパーティション情報が失われました");
    }
    const std::filesystem::path image_path =
        std::filesystem::path(staging) / analyzed->payload_file_name;
    const auto captured = windowsdism::execute_dism_capture(
        windowsdism::DismCaptureRequest{
            .source_root = normalize_capture_root(source.capture_root),
            .image_path = image_path.wstring(),
            .scratch_directory = request.scratch_directory,
            .image_name = L"Y-TEC Tsumugi Drive volume " +
                std::to_wstring(source.source_table_index),
        },
        trusted_system_directory,
        trust_verifier,
        process_runner);
    if (!captured) {
      cleanup_known_staging(staging, created_files);
      return clonecore::Result<ShrinkBundleCaptureReport>::failure(
          captured.error());
    }
    created_files.push_back(image_path.wstring());
    auto payload = hash_regular_file_locked_read_only(image_path.wstring());
    if (!payload) {
      cleanup_known_staging(staging, created_files);
      return clonecore::Result<ShrinkBundleCaptureReport>::failure(
          payload.error());
    }
    if (payload.value().length_bytes == 0U ||
        total_payload > (std::numeric_limits<std::uint64_t>::max)() -
            payload.value().length_bytes) {
      cleanup_known_staging(staging, created_files);
      return failure<ShrinkBundleCaptureReport>(
          clonecore::ErrorCode::io_failed,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小イメージWIM長",
          L"WIMが空か、合計長がオーバーフローしました");
    }
    partition->payload_length_bytes = payload.value().length_bytes;
    partition->payload_sha256 = payload.value().sha256;
    total_payload += payload.value().length_bytes;
  }
  const auto manifest_bytes =
      imageformat::build_shrink_image_manifest_v1(manifest);
  if (!manifest_bytes) {
    cleanup_known_staging(staging, created_files);
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(
        manifest_bytes.error());
  }
  const std::wstring manifest_path =
      (std::filesystem::path(staging) / kShrinkBundleManifestFileName).wstring();
  const auto written = write_new_file(manifest_path, manifest_bytes.value());
  if (!written) {
    cleanup_known_staging(staging, created_files);
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(
        written.error());
  }
  created_files.push_back(manifest_path);
  imageformat::Sha256Digest manifest_hash{};
  {
    auto verified = verify_shrink_bundle_read_only(manifest_path);
    if (!verified) {
      cleanup_known_staging(staging, created_files);
      return clonecore::Result<ShrinkBundleCaptureReport>::failure(
          verified.error());
    }
    manifest_hash = verified.value().manifest_sha256;
  }
  if (!MoveFileExW(
          staging.c_str(),
          request.final_bundle_directory.c_str(),
          MOVEFILE_WRITE_THROUGH)) {
    const auto error = clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"縮小イメージ完成名確定",
        GetLastError());
    cleanup_known_staging(staging, created_files);
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(error);
  }
  const std::wstring final_manifest =
      (std::filesystem::path(request.final_bundle_directory) /
       kShrinkBundleManifestFileName)
          .wstring();
  const auto final_verified = verify_shrink_bundle_read_only(final_manifest);
  if (!final_verified || final_verified.value().manifest_sha256 != manifest_hash) {
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(
        final_verified
            ? capture_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"縮小イメージ完成名再検証",
                  L"完成名確定後にマニフェストSHA-256が変化しました")
            : final_verified.error());
  }
  return clonecore::Result<ShrinkBundleCaptureReport>::success(
      ShrinkBundleCaptureReport{
          .bundle_directory = request.final_bundle_directory,
          .manifest_path = final_manifest,
          .total_payload_bytes = total_payload,
          .captured_volume_count =
              static_cast<std::uint32_t>(request.capture_sources.size()),
          .manifest_sha256 = manifest_hash,
          .committed_after_complete_verification = true,
      });
}

clonecore::Result<ShrinkBundleCaptureReport>
capture_shrink_bundle_with_windows_apis(
    const ShrinkBundleCaptureRequest& request) {
  const auto system_directory = query_system_directory();
  if (!system_directory.has_value()) {
    return clonecore::Result<ShrinkBundleCaptureReport>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"縮小イメージSystem32取得",
            GetLastError()));
  }
  auto trust = bootrepair::make_windows_authenticode_verifier();
  auto process =
      bootrepair::make_windows_process_runner(kDismTimeoutMilliseconds);
  return capture_shrink_bundle(
      request, *system_directory, *trust, *process);
}

}  // namespace ytec::migrationengine

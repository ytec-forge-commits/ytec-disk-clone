#include "ytec/vssrequester/workflow.h"

#include <vsserror.h>

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <utility>

namespace ytec::vssrequester {
namespace {

clonecore::Error workflow_error(
    const clonecore::ErrorCode code,
    const HRESULT status_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = static_cast<DWORD>(status_code),
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
         std::equal(
             left.begin(),
             left.end(),
             right.begin(),
             [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
             });
}

bool is_hex(const wchar_t value) {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

bool is_guid_string(const std::wstring_view value) {
  if (value.size() != 38U || value.front() != L'{' ||
      value.back() != L'}') {
    return false;
  }
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    const bool hyphen =
        index == 9U || index == 14U || index == 19U || index == 24U;
    if ((hyphen && value[index] != L'-') ||
        (!hyphen && !is_hex(value[index]))) {
      return false;
    }
  }
  return true;
}

bool is_volume_guid_path(const std::wstring_view path) {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (path.size() != 49 || !path.starts_with(prefix) ||
      path[47] != L'}' || path[48] != L'\\') {
    return false;
  }
  for (std::size_t index = prefix.size(); index < 47; ++index) {
    const std::size_t guid_index = index - prefix.size();
    const bool expects_hyphen =
        guid_index == 8 || guid_index == 13 || guid_index == 18 ||
        guid_index == 23;
    if ((expects_hyphen && path[index] != L'-') ||
        (!expects_hyphen && !is_hex(path[index]))) {
      return false;
    }
  }
  return true;
}

bool is_snapshot_device_path(const std::wstring_view path) {
  constexpr std::wstring_view prefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (!path.starts_with(prefix) || path.size() <= prefix.size()) {
    return false;
  }
  std::wstring_view suffix = path.substr(prefix.size());
  if (suffix.ends_with(L'\\')) {
    suffix.remove_suffix(1);
  }
  return !suffix.empty() &&
         std::all_of(suffix.begin(), suffix.end(), [](const wchar_t value) {
           return value >= L'0' && value <= L'9';
         });
}

clonecore::Status validate_request(const WorkflowRequest& request) {
  if (!request.administrator) {
    return clonecore::Status::failure(workflow_error(
        clonecore::ErrorCode::access_denied,
        E_ACCESSDENIED,
        L"VSS管理者権限確認",
        L"VSSバックアップは管理者トークンでだけ開始できます"));
  }
  if (request.volumes.empty() || request.volumes.size() > 128) {
    return clonecore::Status::failure(workflow_error(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS対象ボリューム検証",
        L"1個以上128個以下の対象ボリュームが必要です"));
  }
  for (std::size_t index = 0; index < request.volumes.size(); ++index) {
    const auto& volume = request.volumes[index];
    if (!is_volume_guid_path(volume.volume_guid_path) ||
        !equals_case_insensitive(volume.file_system, L"NTFS")) {
      return clonecore::Status::failure(workflow_error(
          clonecore::ErrorCode::unsupported_layout,
          E_INVALIDARG,
          L"VSS対象ボリューム検証",
          L"正規のVolume GUIDパスを持つNTFSだけを対象にできます"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (equals_case_insensitive(
              volume.volume_guid_path,
              request.volumes[previous].volume_guid_path)) {
        return clonecore::Status::failure(workflow_error(
            clonecore::ErrorCode::invalid_argument,
            E_INVALIDARG,
            L"VSS対象ボリューム重複検証",
            L"同じボリュームをSnapshot setへ重複追加できません"));
      }
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_writers(
    const std::vector<WriterStatus>& writers) {
  if (writers.empty()) {
    return clonecore::Status::failure(workflow_error(
        clonecore::ErrorCode::verification_failed,
        VSS_E_BAD_STATE,
        L"VSS Writer状態確認",
        L"Writer状態を1件も確認できないため処理を継続できません"));
  }
  for (const auto& writer : writers) {
    const bool valid_post_snapshot_state =
        writer.state == WriterState::stable ||
        writer.state == WriterState::waiting_for_backup_complete;
    if (writer.name.empty() || !valid_post_snapshot_state ||
        writer.status_code != S_OK) {
      std::wstring message = L"VSS Writerが安定状態ではありません";
      if (!writer.name.empty()) {
        message += L": " + writer.name;
      }
      return clonecore::Status::failure(workflow_error(
          clonecore::ErrorCode::verification_failed,
          writer.status_code,
          L"VSS Writer状態確認",
          std::move(message)));
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_mappings(
    const std::vector<VolumeRequest>& volumes,
    const std::vector<SnapshotMapping>& mappings) {
  if (mappings.size() != volumes.size()) {
    return clonecore::Status::failure(workflow_error(
        clonecore::ErrorCode::verification_failed,
        VSS_E_OBJECT_NOT_FOUND,
        L"VSS Snapshotデバイス対応確認",
        L"対象ボリュームとSnapshotデバイスの件数が一致しません"));
  }
  for (const auto& volume : volumes) {
    const auto matches = std::count_if(
        mappings.begin(), mappings.end(), [&](const auto& mapping) {
          return equals_case_insensitive(
              mapping.original_volume_guid_path,
              volume.volume_guid_path);
        });
    if (matches != 1) {
      return clonecore::Status::failure(workflow_error(
          clonecore::ErrorCode::identity_mismatch,
          VSS_E_OBJECT_NOT_FOUND,
          L"VSS Snapshotデバイス対応確認",
          L"各対象ボリュームには一意なSnapshotが必要です"));
    }
  }
  for (std::size_t index = 0; index < mappings.size(); ++index) {
    if (!is_volume_guid_path(mappings[index].original_volume_guid_path) ||
        !is_guid_string(mappings[index].snapshot_id) ||
        !is_snapshot_device_path(mappings[index].snapshot_device_path) ||
        !is_guid_string(mappings[index].provider_id) ||
        mappings[index].creation_timestamp == 0) {
      return clonecore::Status::failure(workflow_error(
          clonecore::ErrorCode::invalid_data,
        E_INVALIDARG,
        L"VSS Snapshotデバイスパス検証",
        L"VSSが返したSnapshot、デバイス、provider、またはcreation timestampが不正です"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (equals_case_insensitive(
              mappings[index].snapshot_id,
              mappings[previous].snapshot_id) ||
          equals_case_insensitive(
              mappings[index].snapshot_device_path,
              mappings[previous].snapshot_device_path)) {
        return clonecore::Status::failure(workflow_error(
            clonecore::ErrorCode::identity_mismatch,
            E_INVALIDARG,
            L"VSS Snapshotデバイス重複検証",
            L"複数ボリュームが同じSnapshotデバイスへ対応しています"));
      }
    }
  }
  return clonecore::success_status();
}

clonecore::Result<WorkflowReport> fail_after_snapshot_set(
    clonecore::Error primary_error,
    const std::wstring& snapshot_set_id,
    IWorkflowBackend& backend) {
  const auto cleanup = backend.delete_snapshot_set(snapshot_set_id);
  if (!cleanup) {
    primary_error.message +=
        L"; Snapshot削除にも失敗: " + cleanup.error().message;
  }
  return clonecore::Result<WorkflowReport>::failure(
      std::move(primary_error));
}

}  // namespace

clonecore::Result<WorkflowReport> execute_backup_workflow(
    const WorkflowRequest& request,
    IWorkflowBackend& backend) {
  const auto request_status = validate_request(request);
  if (!request_status) {
    return clonecore::Result<WorkflowReport>::failure(
        request_status.error());
  }
  const auto initialized = backend.initialize_components();
  if (!initialized) {
    return clonecore::Result<WorkflowReport>::failure(initialized.error());
  }
  const auto backup_state = backend.set_backup_state();
  if (!backup_state) {
    return clonecore::Result<WorkflowReport>::failure(backup_state.error());
  }
  const auto metadata = backend.gather_writer_metadata();
  if (!metadata) {
    return clonecore::Result<WorkflowReport>::failure(metadata.error());
  }
  auto snapshot_set = backend.start_snapshot_set();
  if (!snapshot_set) {
    return clonecore::Result<WorkflowReport>::failure(snapshot_set.error());
  }
  std::wstring snapshot_set_id = snapshot_set.take_value();
  if (snapshot_set_id.empty()) {
    return fail_after_snapshot_set(
        workflow_error(
            clonecore::ErrorCode::invalid_data,
            VSS_E_BAD_STATE,
            L"VSS Snapshot set識別子検証",
            L"空のSnapshot set識別子は使用できません"),
        snapshot_set_id,
        backend);
  }

  for (const auto& volume : request.volumes) {
    const auto added =
        backend.add_volume(snapshot_set_id, volume.volume_guid_path);
    if (!added) {
      return fail_after_snapshot_set(
          added.error(), snapshot_set_id, backend);
    }
  }
  const auto prepared = backend.prepare_for_backup();
  if (!prepared) {
    return fail_after_snapshot_set(
        prepared.error(), snapshot_set_id, backend);
  }
  const auto snapshotted = backend.do_snapshot_set();
  if (!snapshotted) {
    return fail_after_snapshot_set(
        snapshotted.error(), snapshot_set_id, backend);
  }
  const auto writer_result = backend.query_writer_statuses();
  if (!writer_result) {
    return fail_after_snapshot_set(
        writer_result.error(), snapshot_set_id, backend);
  }
  const auto writer_status = validate_writers(writer_result.value());
  if (!writer_status) {
    return fail_after_snapshot_set(
        writer_status.error(), snapshot_set_id, backend);
  }
  const auto mapping_result =
      backend.query_snapshot_devices(snapshot_set_id, request.volumes);
  if (!mapping_result) {
    return fail_after_snapshot_set(
        mapping_result.error(), snapshot_set_id, backend);
  }
  const auto mapping_status =
      validate_mappings(request.volumes, mapping_result.value());
  if (!mapping_status) {
    return fail_after_snapshot_set(
        mapping_status.error(), snapshot_set_id, backend);
  }
  const auto copied = backend.copy_snapshot_data(mapping_result.value());
  if (!copied) {
    return fail_after_snapshot_set(
        copied.error(), snapshot_set_id, backend);
  }
  const auto completed = backend.backup_complete();
  if (!completed) {
    return fail_after_snapshot_set(
        completed.error(), snapshot_set_id, backend);
  }
  const auto deleted = backend.delete_snapshot_set(snapshot_set_id);
  if (!deleted) {
    return clonecore::Result<WorkflowReport>::failure(deleted.error());
  }
  return clonecore::Result<WorkflowReport>::success(WorkflowReport{
      .snapshot_set_id = std::move(snapshot_set_id),
      .volume_count = request.volumes.size(),
      .writer_count = writer_result.value().size(),
      .snapshot_data_copied = true,
      .backup_completed = true,
      .snapshots_deleted = true,
  });
}

}  // namespace ytec::vssrequester

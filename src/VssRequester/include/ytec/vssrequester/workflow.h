#pragma once

#include "ytec/clonecore/result.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::vssrequester {

enum class WriterState : std::uint8_t {
  stable,
  waiting_for_backup_complete,
  failed,
  unknown,
};

struct WriterStatus final {
  std::wstring name;
  WriterState state{WriterState::unknown};
  HRESULT status_code{E_UNEXPECTED};
};

struct VolumeRequest final {
  std::wstring volume_guid_path;
  std::wstring file_system;
};

struct SnapshotMapping final {
  std::wstring original_volume_guid_path;
  std::wstring snapshot_id;
  std::wstring snapshot_device_path;
  // Captured from GetSnapshotProperties together with the Snapshot device.
  // Product safety monitors bind both values immutably so a provider or
  // same-ID Snapshot generation cannot be substituted between polls.
  std::wstring provider_id;
  std::int64_t creation_timestamp{};
};

struct SnapshotCopyContext final {
  std::wstring snapshot_set_id;
  std::vector<SnapshotMapping> mappings;
};

struct WorkflowRequest final {
  bool administrator{};
  std::vector<VolumeRequest> volumes;
};

struct WorkflowReport final {
  std::wstring snapshot_set_id;
  std::size_t volume_count{};
  std::size_t writer_count{};
  bool snapshot_data_copied{};
  bool backup_completed{};
  bool snapshots_deleted{};
};

class IWorkflowBackend {
 public:
  virtual ~IWorkflowBackend() = default;

  [[nodiscard]] virtual clonecore::Status initialize_components() = 0;
  [[nodiscard]] virtual clonecore::Status set_backup_state() = 0;
  [[nodiscard]] virtual clonecore::Status gather_writer_metadata() = 0;
  [[nodiscard]] virtual clonecore::Result<std::wstring>
  start_snapshot_set() = 0;
  [[nodiscard]] virtual clonecore::Status add_volume(
      const std::wstring& snapshot_set_id,
      const std::wstring& volume_guid_path) = 0;
  [[nodiscard]] virtual clonecore::Status prepare_for_backup() = 0;
  [[nodiscard]] virtual clonecore::Status do_snapshot_set() = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<WriterStatus>>
  query_writer_statuses() = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<SnapshotMapping>>
  query_snapshot_devices(
      const std::wstring& snapshot_set_id,
      const std::vector<VolumeRequest>& volumes) = 0;
  [[nodiscard]] virtual clonecore::Status copy_snapshot_data(
      const std::vector<SnapshotMapping>& mappings) = 0;
  [[nodiscard]] virtual clonecore::Status backup_complete() = 0;
  [[nodiscard]] virtual clonecore::Status delete_snapshot_set(
      const std::wstring& snapshot_set_id) = 0;
};

[[nodiscard]] clonecore::Result<WorkflowReport> execute_backup_workflow(
    const WorkflowRequest& request,
    IWorkflowBackend& backend);

}  // namespace ytec::vssrequester

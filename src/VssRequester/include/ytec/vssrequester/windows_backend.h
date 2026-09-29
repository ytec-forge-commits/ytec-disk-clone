#pragma once

#include "ytec/clonecore/log.h"
#include "ytec/clonecore/result.h"
#include "ytec/vssrequester/workflow.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::vssrequester {

struct AsyncWaitOptions final {
  DWORD timeout_ms{120'000};
  DWORD poll_interval_ms{250};
  std::function<bool()> cancellation_requested;
};

class IVssAsyncOperation {
 public:
  virtual ~IVssAsyncOperation() = default;

  [[nodiscard]] virtual HRESULT wait(DWORD milliseconds) noexcept = 0;
  [[nodiscard]] virtual HRESULT query_status(
      HRESULT* operation_status) noexcept = 0;
  [[nodiscard]] virtual HRESULT cancel() noexcept = 0;
};

[[nodiscard]] clonecore::Status wait_for_vss_async(
    IVssAsyncOperation& operation,
    const AsyncWaitOptions& options,
    std::wstring_view operation_name);

// VSSが返したSnapshot Set、Snapshot ID、元Volume、デバイスパスを
// 相互再検証した完全なContextだけを渡します。
using SnapshotCopyCallback = std::function<clonecore::Status(
    const SnapshotCopyContext& context)>;

struct WindowsVssBackendOptions final {
  AsyncWaitOptions async_wait;
  SnapshotCopyCallback copy_snapshot_data;
  const clonecore::Logger* logger{};
};

// Process-wide COM security must be fixed before another COM client causes
// implicit initialization. The Windows application calls this during startup;
// the VSS backend calls it again as an idempotent safety check.
[[nodiscard]] clonecore::Status initialize_vss_process_security();

// 1インスタンスを1バックアップ処理の同一スレッド内だけで使用します。
// 実VSSを呼ぶため、管理者権限とWindows VSSサービスが必要です。
class WindowsVssBackend final : public IWorkflowBackend {
 public:
  explicit WindowsVssBackend(WindowsVssBackendOptions options);
  ~WindowsVssBackend() override;

  WindowsVssBackend(const WindowsVssBackend&) = delete;
  WindowsVssBackend& operator=(const WindowsVssBackend&) = delete;
  WindowsVssBackend(WindowsVssBackend&&) = delete;
  WindowsVssBackend& operator=(WindowsVssBackend&&) = delete;

  [[nodiscard]] clonecore::Status initialize_components() override;
  [[nodiscard]] clonecore::Status set_backup_state() override;
  [[nodiscard]] clonecore::Status gather_writer_metadata() override;
  [[nodiscard]] clonecore::Result<std::wstring>
  start_snapshot_set() override;
  [[nodiscard]] clonecore::Status add_volume(
      const std::wstring& snapshot_set_id,
      const std::wstring& volume_guid_path) override;
  [[nodiscard]] clonecore::Status prepare_for_backup() override;
  [[nodiscard]] clonecore::Status do_snapshot_set() override;
  [[nodiscard]] clonecore::Result<std::vector<WriterStatus>>
  query_writer_statuses() override;
  [[nodiscard]] clonecore::Result<std::vector<SnapshotMapping>>
  query_snapshot_devices(
      const std::wstring& snapshot_set_id,
      const std::vector<VolumeRequest>& volumes) override;
  [[nodiscard]] clonecore::Status copy_snapshot_data(
      const std::vector<SnapshotMapping>& mappings) override;
  [[nodiscard]] clonecore::Status backup_complete() override;
  [[nodiscard]] clonecore::Status delete_snapshot_set(
      const std::wstring& snapshot_set_id) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ytec::vssrequester

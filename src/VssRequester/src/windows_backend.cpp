#include "ytec/vssrequester/windows_backend.h"

#include <vss.h>
#include <vsserror.h>
#include <vswriter.h>
#include <vsbackup.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace ytec::vssrequester {
namespace {

clonecore::Error vss_error(
    const clonecore::ErrorCode code,
    const HRESULT native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = static_cast<DWORD>(native_code),
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Status fail(
    const clonecore::ErrorCode code,
    const HRESULT native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(vss_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

std::wstring hresult_text(const HRESULT value) {
  std::wostringstream stream;
  stream << L"0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill(L'0') << static_cast<DWORD>(value);
  return stream.str();
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
         std::all_of(
             suffix.begin(),
             suffix.end(),
             [](const wchar_t value) {
               return value >= L'0' && value <= L'9';
             });
}

bool guid_is_null(const VSS_ID& value) noexcept {
  return IsEqualGUID(value, GUID_NULL) != FALSE;
}

std::wstring guid_to_string(const VSS_ID& value) {
  std::array<wchar_t, 39> buffer{};
  if (StringFromGUID2(
          value,
          buffer.data(),
          static_cast<int>(buffer.size())) == 0) {
    return {};
  }
  return buffer.data();
}

template <typename Interface>
class ComPtr final {
 public:
  ComPtr() = default;
  ~ComPtr() { reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  ComPtr(ComPtr&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}

  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] Interface* get() const noexcept { return value_; }
  [[nodiscard]] Interface* operator->() const noexcept { return value_; }

  [[nodiscard]] Interface** put() noexcept {
    reset();
    return &value_;
  }

  void attach(Interface* value) noexcept {
    reset();
    value_ = value;
  }

  void reset() noexcept {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  Interface* value_{};
};

class Bstr final {
 public:
  Bstr() = default;
  ~Bstr() { SysFreeString(value_); }

  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;

  [[nodiscard]] BSTR* put() noexcept {
    SysFreeString(value_);
    value_ = nullptr;
    return &value_;
  }

  [[nodiscard]] BSTR get() const noexcept { return value_; }

 private:
  BSTR value_{};
};

class SnapshotProperties final {
 public:
  SnapshotProperties() = default;
  ~SnapshotProperties() {
    VssFreeSnapshotProperties(&value_);
  }

  SnapshotProperties(const SnapshotProperties&) = delete;
  SnapshotProperties& operator=(const SnapshotProperties&) = delete;

  [[nodiscard]] VSS_SNAPSHOT_PROP* put() noexcept { return &value_; }
  [[nodiscard]] const VSS_SNAPSHOT_PROP& get() const noexcept {
    return value_;
  }

 private:
  VSS_SNAPSHOT_PROP value_{};
};

class VssAsyncAdapter final : public IVssAsyncOperation {
 public:
  explicit VssAsyncAdapter(IVssAsync* operation) : operation_(operation) {}

  [[nodiscard]] HRESULT wait(
      const DWORD milliseconds) noexcept override {
    return operation_ == nullptr ? E_POINTER
                                 : operation_->Wait(milliseconds);
  }

  [[nodiscard]] HRESULT query_status(
      HRESULT* const operation_status) noexcept override {
    return operation_ == nullptr
               ? E_POINTER
               : operation_->QueryStatus(operation_status, nullptr);
  }

  [[nodiscard]] HRESULT cancel() noexcept override {
    return operation_ == nullptr ? E_POINTER : operation_->Cancel();
  }

 private:
  IVssAsync* operation_{};
};

clonecore::Status cancel_and_fail(
    IVssAsyncOperation& operation,
    const HRESULT native_code,
    const std::wstring_view operation_name,
    std::wstring message) {
  const HRESULT cancel_result = operation.cancel();
  if (FAILED(cancel_result)) {
    message += L"; Cancel失敗=" + hresult_text(cancel_result);
  }
  return fail(
      clonecore::ErrorCode::query_failed,
      native_code,
      std::wstring(operation_name),
      std::move(message));
}

clonecore::Status check_step(
    const bool condition,
    std::wstring operation,
    std::wstring message) {
  if (condition) {
    return clonecore::success_status();
  }
  return fail(
      clonecore::ErrorCode::internal_error,
      VSS_E_BAD_STATE,
      std::move(operation),
      std::move(message));
}

struct ProcessSecurityState final {
  std::once_flag once;
  HRESULT result{E_UNEXPECTED};
};

ProcessSecurityState& process_security_state() {
  static ProcessSecurityState state;
  return state;
}

clonecore::Status wait_on_com_async(
    IVssAsync* const operation,
    const AsyncWaitOptions& options,
    const std::wstring_view operation_name) {
  if (operation == nullptr) {
    return fail(
        clonecore::ErrorCode::invalid_data,
        E_POINTER,
        std::wstring(operation_name),
        L"VSSが非同期操作オブジェクトを返しませんでした");
  }
  VssAsyncAdapter adapter(operation);
  return wait_for_vss_async(adapter, options, operation_name);
}

}  // namespace

clonecore::Status initialize_vss_process_security() {
  auto& state = process_security_state();
  std::call_once(state.once, [&state]() {
    state.result = CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IDENTIFY,
        nullptr,
        EOAC_NONE,
        nullptr);
  });
  if (FAILED(state.result)) {
    return fail(
        clonecore::ErrorCode::access_denied,
        state.result,
        L"VSS COMセキュリティ初期化",
        L"CoInitializeSecurityを要求した認証・偽装レベルで初期化できません");
  }
  return clonecore::success_status();
}

clonecore::Status wait_for_vss_async(
    IVssAsyncOperation& operation,
    const AsyncWaitOptions& options,
    const std::wstring_view operation_name) {
  if (operation_name.empty() || options.timeout_ms == 0 ||
      options.timeout_ms == INFINITE ||
      options.poll_interval_ms == 0 ||
      options.poll_interval_ms == INFINITE) {
    return fail(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS非同期待機設定",
        L"操作名、有限のtimeout、poll間隔が必要です");
  }

  DWORD elapsed = 0;
  while (elapsed < options.timeout_ms) {
    if (options.cancellation_requested &&
        options.cancellation_requested()) {
      return cancel_and_fail(
          operation,
          HRESULT_FROM_WIN32(ERROR_CANCELLED),
          operation_name,
          L"キャンセル要求を受けたためVSS非同期操作を中止しました");
    }

    const DWORD remaining = options.timeout_ms - elapsed;
    const DWORD wait_slice =
        (std::min)(remaining, options.poll_interval_ms);
    const HRESULT wait_result = operation.wait(wait_slice);
    if (FAILED(wait_result)) {
      return cancel_and_fail(
          operation,
          wait_result,
          operation_name,
          L"VSS非同期待機に失敗しました: " +
              hresult_text(wait_result));
    }

    HRESULT operation_result = VSS_S_ASYNC_PENDING;
    const HRESULT query_result =
        operation.query_status(&operation_result);
    if (FAILED(query_result)) {
      return cancel_and_fail(
          operation,
          query_result,
          operation_name,
          L"VSS非同期状態の取得に失敗しました: " +
              hresult_text(query_result));
    }
    if (operation_result == S_OK ||
        operation_result == VSS_S_ASYNC_FINISHED) {
      return clonecore::success_status();
    }
    if (operation_result == VSS_S_ASYNC_CANCELLED) {
      return fail(
          clonecore::ErrorCode::query_failed,
          HRESULT_FROM_WIN32(ERROR_CANCELLED),
          std::wstring(operation_name),
          L"VSS非同期操作がキャンセル状態で終了しました");
    }
    if (FAILED(operation_result)) {
      return fail(
          clonecore::ErrorCode::query_failed,
          operation_result,
          std::wstring(operation_name),
          L"VSS非同期操作が失敗しました: " +
              hresult_text(operation_result));
    }
    if (operation_result != VSS_S_ASYNC_PENDING) {
      return cancel_and_fail(
          operation,
          E_UNEXPECTED,
          operation_name,
          L"VSS非同期操作が未知の成功状態を返しました: " +
              hresult_text(operation_result));
    }
    elapsed += wait_slice;
  }

  return cancel_and_fail(
      operation,
      HRESULT_FROM_WIN32(ERROR_TIMEOUT),
      operation_name,
      L"有限時間内にVSS非同期操作が完了しなかったため中止しました");
}

class WindowsVssBackend::Impl final {
 public:
  enum class Step : std::uint8_t {
    created,
    initialized,
    backup_state_set,
    metadata_gathered,
    snapshot_set_started,
    volumes_added,
    prepared,
    snapshotted,
    data_copied,
    backup_completed,
    cleaned,
  };

  struct SnapshotIdentity final {
    std::wstring original_volume;
    VSS_ID snapshot_id{GUID_NULL};
  };

  explicit Impl(WindowsVssBackendOptions supplied_options)
      : options(std::move(supplied_options)) {}

  ~Impl() {
    cleanup_best_effort();
    components.reset();
    if (com_initialized) {
      CoUninitialize();
      com_initialized = false;
    }
  }

  clonecore::Status require_step(
      const Step expected,
      std::wstring operation) const {
    return check_step(
        step == expected,
        std::move(operation),
        L"VSSバックエンドが規定外の順序で呼び出されました");
  }

  void log_info(const std::wstring_view message) const {
    if (options.logger != nullptr) {
      options.logger->info(message);
    }
  }

  void log_warning(const std::wstring_view message) const {
    if (options.logger != nullptr) {
      options.logger->warning(message);
    }
  }

  clonecore::Status wait_async(
      IVssAsync* const operation,
      const std::wstring_view name) const {
    return wait_on_com_async(operation, options.async_wait, name);
  }

  clonecore::Status collect_writer_statuses(
      std::vector<WriterStatus>& writers,
      const bool require_stable) {
    ComPtr<IVssAsync> async;
    HRESULT result = components->GatherWriterStatus(async.put());
    if (FAILED(result)) {
      return fail(
          clonecore::ErrorCode::query_failed,
          result,
          L"VSS Writer状態収集開始",
          L"GatherWriterStatusに失敗しました");
    }
    const auto waited = wait_async(async.get(), L"VSS Writer状態収集待機");
    if (!waited) {
      return waited;
    }

    UINT count = 0;
    result = components->GetWriterStatusCount(&count);
    if (FAILED(result)) {
      return fail(
          clonecore::ErrorCode::query_failed,
          result,
          L"VSS Writer状態件数取得",
          L"GetWriterStatusCountに失敗しました");
    }

    struct WriterStatusGuard final {
      IVssBackupComponents* components{};
      ~WriterStatusGuard() {
        if (components != nullptr) {
          static_cast<void>(components->FreeWriterStatus());
        }
      }
    } guard{components.get()};

    writers.clear();
    writers.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      VSS_ID instance_id = GUID_NULL;
      VSS_ID writer_id = GUID_NULL;
      Bstr name;
      VSS_WRITER_STATE state = VSS_WS_UNKNOWN;
      HRESULT failure = E_UNEXPECTED;
      result = components->GetWriterStatus(
          index,
          &instance_id,
          &writer_id,
          name.put(),
          &state,
          &failure);
      if (FAILED(result)) {
        return fail(
            clonecore::ErrorCode::query_failed,
            result,
            L"VSS Writer状態取得",
            L"GetWriterStatusに失敗しました");
      }

      const std::wstring writer_name =
          name.get() == nullptr ? L"" : std::wstring(name.get());
      WriterState mapped_state = WriterState::failed;
      if (state == VSS_WS_STABLE) {
        mapped_state = WriterState::stable;
      } else if (state == VSS_WS_WAITING_FOR_BACKUP_COMPLETE) {
        mapped_state = WriterState::waiting_for_backup_complete;
      }
      writers.push_back(WriterStatus{
          .name = writer_name,
          .state = mapped_state,
          .status_code = failure,
      });

      log_info(
          L"VSS Writer: name=" + writer_name +
          L", instance=" + guid_to_string(instance_id) +
          L", writer=" + guid_to_string(writer_id) +
          L", state=" + std::to_wstring(static_cast<int>(state)) +
          L", HRESULT=" + hresult_text(failure));

      if (require_stable &&
          (writer_name.empty() || state != VSS_WS_STABLE ||
           failure != S_OK)) {
        return fail(
            clonecore::ErrorCode::verification_failed,
            failure,
            L"VSS BackupComplete後Writer状態確認",
            L"BackupComplete後に安定状態ではないWriterがあります: " +
                writer_name);
      }
    }
    if (writers.empty()) {
      return fail(
          clonecore::ErrorCode::verification_failed,
          VSS_E_BAD_STATE,
          L"VSS Writer状態確認",
          L"Writer状態を1件も取得できませんでした");
    }
    return clonecore::success_status();
  }

  clonecore::Status delete_exact_snapshot_set() {
    if (components.get() == nullptr || guid_is_null(snapshot_set_id)) {
      return clonecore::success_status();
    }

    // Once DoSnapshotSet has completed, delete the exact non-persistent set
    // before notifying writers that a failed backup is being aborted.
    // AbortBackup can release the set itself; calling it first made the
    // following exact DeleteSnapshots call report VSS_E_OBJECT_NOT_FOUND even
    // though no shadow copy remained.
    if (snapshots_created) {
      LONG deleted = 0;
      VSS_ID not_deleted = GUID_NULL;
      const HRESULT delete_result = components->DeleteSnapshots(
          snapshot_set_id,
          VSS_OBJECT_SNAPSHOT_SET,
          FALSE,
          &deleted,
          &not_deleted);
      if (FAILED(delete_result)) {
        return fail(
            clonecore::ErrorCode::io_failed,
            delete_result,
            L"VSS Snapshot set削除",
            L"作成したSnapshot setだけを削除できませんでした; "
            L"nonDeleted=" +
                guid_to_string(not_deleted));
      }
      if (deleted != static_cast<LONG>(snapshots.size())) {
        return fail(
            clonecore::ErrorCode::verification_failed,
            VSS_E_OBJECT_NOT_FOUND,
            L"VSS Snapshot set削除件数確認",
            L"作成したSnapshot件数と削除件数が一致しません");
      }
      log_info(
          L"VSS Snapshot set削除完了: count=" +
          std::to_wstring(deleted));
    }

    snapshot_set_id = GUID_NULL;
    snapshots.clear();
    verified_mappings.clear();
    snapshots_created = false;

    if (!backup_complete_notified && step != Step::cleaned) {
      const HRESULT abort_result = components->AbortBackup();
      if (FAILED(abort_result)) {
        return fail(
            clonecore::ErrorCode::io_failed,
            abort_result,
            L"VSS Backup中断通知",
            L"Snapshot削除後にAbortBackupを完了できませんでした");
      }
      log_info(L"VSS Backup中断をWriterへ通知しました");
    }

    backup_document_initialized = false;
    step = Step::cleaned;
    return clonecore::success_status();
  }

  void cleanup_best_effort() noexcept {
    if (components.get() == nullptr) {
      return;
    }
    if (!guid_is_null(snapshot_set_id)) {
      const auto cleaned = delete_exact_snapshot_set();
      if (!cleaned) {
        log_warning(
            L"VSSデストラクタCleanup失敗: " + cleaned.error().message);
      }
    } else if (backup_document_initialized &&
               !backup_complete_notified &&
               step != Step::cleaned) {
      const HRESULT result = components->AbortBackup();
      if (FAILED(result)) {
        log_warning(
            L"VSSデストラクタAbortBackup失敗: " +
            hresult_text(result));
      }
      backup_document_initialized = false;
    }
    if (metadata_available) {
      const HRESULT result = components->FreeWriterMetadata();
      if (FAILED(result)) {
        log_warning(
            L"VSSデストラクタWriterメタデータ解放失敗: " +
            hresult_text(result));
      }
      metadata_available = false;
    }
  }

  WindowsVssBackendOptions options;
  ComPtr<IVssBackupComponents> components;
  bool com_initialized{};
  bool backup_document_initialized{};
  bool backup_complete_notified{};
  bool metadata_available{};
  bool snapshots_created{};
  Step step{Step::created};
  VSS_ID snapshot_set_id{GUID_NULL};
  std::vector<SnapshotIdentity> snapshots;
  std::vector<SnapshotMapping> verified_mappings;
};

WindowsVssBackend::WindowsVssBackend(WindowsVssBackendOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

WindowsVssBackend::~WindowsVssBackend() = default;

clonecore::Status WindowsVssBackend::initialize_components() {
  const auto ordered =
      impl_->require_step(Impl::Step::created, L"VSS初期化順序確認");
  if (!ordered) {
    return ordered;
  }
  if (!impl_->options.copy_snapshot_data) {
    return fail(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS Snapshotコピー境界確認",
        L"Snapshot専用コピーコールバックが設定されていません");
  }
  if (impl_->options.async_wait.timeout_ms == 0 ||
      impl_->options.async_wait.timeout_ms == INFINITE ||
      impl_->options.async_wait.poll_interval_ms == 0 ||
      impl_->options.async_wait.poll_interval_ms == INFINITE) {
    return fail(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS非同期待機設定",
        L"有限のtimeoutとpoll間隔が必要です");
  }

  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::internal_error,
        result,
        L"VSS COM初期化",
        L"マルチスレッドCOMを初期化できませんでした");
  }
  impl_->com_initialized = true;

  IGlobalOptions* global_options_raw = nullptr;
  result = CoCreateInstance(
      CLSID_GlobalOptions,
      nullptr,
      CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&global_options_raw));
  ComPtr<IGlobalOptions> global_options;
  global_options.attach(global_options_raw);
  if (FAILED(result) || global_options.get() == nullptr) {
    return fail(
        clonecore::ErrorCode::internal_error,
        FAILED(result) ? result : E_POINTER,
        L"VSS COM例外処理設定",
        L"IGlobalOptionsを取得できませんでした");
  }
  result = global_options->Set(
      COMGLB_EXCEPTION_HANDLING,
      COMGLB_EXCEPTION_DONOT_HANDLE);
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::internal_error,
        result,
        L"VSS COM例外処理設定",
        L"COMが致命的例外を隠さない設定を適用できませんでした");
  }

  const auto security = initialize_vss_process_security();
  if (!security) {
    return security;
  }

  result = CreateVssBackupComponents(impl_->components.put());
  if (FAILED(result) || impl_->components.get() == nullptr) {
    return fail(
        clonecore::ErrorCode::query_failed,
        FAILED(result) ? result : E_POINTER,
        L"VSS BackupComponents作成",
        L"CreateVssBackupComponentsに失敗しました");
  }
  result = impl_->components->InitializeForBackup(nullptr);
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS BackupComponents初期化",
        L"InitializeForBackupに失敗しました");
  }
  impl_->backup_document_initialized = true;
  result = impl_->components->SetContext(VSS_CTX_BACKUP);
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS Context設定",
        L"VSS_CTX_BACKUPを設定できませんでした");
  }
  impl_->step = Impl::Step::initialized;
  impl_->log_info(L"VSS BackupComponentsをVSS_CTX_BACKUPで初期化しました");
  return clonecore::success_status();
}

clonecore::Status WindowsVssBackend::set_backup_state() {
  const auto ordered =
      impl_->require_step(Impl::Step::initialized, L"VSS BackupState順序確認");
  if (!ordered) {
    return ordered;
  }
  const HRESULT result = impl_->components->SetBackupState(
      false,
      true,
      VSS_BT_FULL,
      false);
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS BackupState設定",
        L"非Component選択のフルバックアップ状態を設定できませんでした");
  }
  impl_->step = Impl::Step::backup_state_set;
  return clonecore::success_status();
}

clonecore::Status WindowsVssBackend::gather_writer_metadata() {
  const auto ordered = impl_->require_step(
      Impl::Step::backup_state_set,
      L"VSS Writerメタデータ順序確認");
  if (!ordered) {
    return ordered;
  }

  ComPtr<IVssAsync> async;
  HRESULT result =
      impl_->components->GatherWriterMetadata(async.put());
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS Writerメタデータ収集開始",
        L"GatherWriterMetadataに失敗しました");
  }
  const auto waited =
      impl_->wait_async(async.get(), L"VSS Writerメタデータ収集待機");
  if (!waited) {
    return waited;
  }
  impl_->metadata_available = true;

  UINT writer_count = 0;
  result = impl_->components->GetWriterMetadataCount(&writer_count);
  if (FAILED(result) || writer_count == 0) {
    return fail(
        clonecore::ErrorCode::verification_failed,
        FAILED(result) ? result : VSS_E_BAD_STATE,
        L"VSS Writerメタデータ件数確認",
        L"Writerメタデータを1件も確認できませんでした");
  }

  for (UINT index = 0; index < writer_count; ++index) {
    VSS_ID instance_id = GUID_NULL;
    ComPtr<IVssExamineWriterMetadata> metadata;
    result = impl_->components->GetWriterMetadata(
        index,
        &instance_id,
        metadata.put());
    if (FAILED(result) || metadata.get() == nullptr) {
      return fail(
          clonecore::ErrorCode::query_failed,
          FAILED(result) ? result : E_POINTER,
          L"VSS Writerメタデータ取得",
          L"GetWriterMetadataに失敗しました");
    }

    VSS_ID identity_instance = GUID_NULL;
    VSS_ID writer_id = GUID_NULL;
    Bstr writer_name;
    VSS_USAGE_TYPE usage = VSS_UT_UNDEFINED;
    VSS_SOURCE_TYPE source = VSS_ST_UNDEFINED;
    result = metadata->GetIdentity(
        &identity_instance,
        &writer_id,
        writer_name.put(),
        &usage,
        &source);
    if (FAILED(result) || writer_name.get() == nullptr ||
        SysStringLen(writer_name.get()) == 0 ||
        !IsEqualGUID(instance_id, identity_instance)) {
      return fail(
          clonecore::ErrorCode::verification_failed,
          FAILED(result) ? result : VSS_E_BAD_STATE,
          L"VSS WriterメタデータIdentity確認",
          L"Writer名またはInstance IDを検証できませんでした");
    }
    impl_->log_info(
        L"VSS Writer metadata: name=" +
        std::wstring(writer_name.get()) +
        L", instance=" + guid_to_string(identity_instance) +
        L", writer=" + guid_to_string(writer_id) +
        L", usage=" +
        std::to_wstring(static_cast<int>(usage)) +
        L", source=" +
        std::to_wstring(static_cast<int>(source)));
  }

  result = impl_->components->FreeWriterMetadata();
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS Writerメタデータ解放",
        L"FreeWriterMetadataに失敗しました");
  }
  impl_->metadata_available = false;
  impl_->step = Impl::Step::metadata_gathered;
  return clonecore::success_status();
}

clonecore::Result<std::wstring>
WindowsVssBackend::start_snapshot_set() {
  const auto ordered = impl_->require_step(
      Impl::Step::metadata_gathered,
      L"VSS Snapshot set開始順序確認");
  if (!ordered) {
    return clonecore::Result<std::wstring>::failure(ordered.error());
  }
  VSS_ID snapshot_set_id = GUID_NULL;
  const HRESULT result =
      impl_->components->StartSnapshotSet(&snapshot_set_id);
  if (FAILED(result) || guid_is_null(snapshot_set_id)) {
    return clonecore::Result<std::wstring>::failure(vss_error(
        clonecore::ErrorCode::query_failed,
        FAILED(result) ? result : VSS_E_BAD_STATE,
        L"VSS Snapshot set開始",
        L"StartSnapshotSetが有効な識別子を返しませんでした"));
  }
  std::wstring identity = guid_to_string(snapshot_set_id);
  if (identity.empty()) {
    return clonecore::Result<std::wstring>::failure(vss_error(
        clonecore::ErrorCode::invalid_data,
        E_UNEXPECTED,
        L"VSS Snapshot set識別子変換",
        L"Snapshot set GUIDを文字列へ変換できませんでした"));
  }
  impl_->snapshot_set_id = snapshot_set_id;
  impl_->step = Impl::Step::snapshot_set_started;
  impl_->log_info(L"VSS Snapshot set開始: " + identity);
  return clonecore::Result<std::wstring>::success(std::move(identity));
}

clonecore::Status WindowsVssBackend::add_volume(
    const std::wstring& snapshot_set_id,
    const std::wstring& volume_guid_path) {
  const bool allowed_step =
      impl_->step == Impl::Step::snapshot_set_started ||
      impl_->step == Impl::Step::volumes_added;
  const auto ordered = check_step(
      allowed_step,
      L"VSS Volume追加順序確認",
      L"Snapshot set開始後にだけVolumeを追加できます");
  if (!ordered) {
    return ordered;
  }
  if (snapshot_set_id != guid_to_string(impl_->snapshot_set_id)) {
    return fail(
        clonecore::ErrorCode::identity_mismatch,
        E_INVALIDARG,
        L"VSS Snapshot set識別子確認",
        L"作成したSnapshot set以外へVolumeを追加できません");
  }
  if (std::any_of(
          impl_->snapshots.begin(),
          impl_->snapshots.end(),
          [&](const auto& entry) {
            return equals_case_insensitive(
                entry.original_volume,
                volume_guid_path);
          })) {
    return fail(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS Volume重複確認",
        L"同じVolumeをSnapshot setへ重複追加できません");
  }

  BOOL supported = FALSE;
  HRESULT result = impl_->components->IsVolumeSupported(
      GUID_NULL,
      const_cast<VSS_PWSZ>(volume_guid_path.c_str()),
      &supported);
  if (FAILED(result) || supported == FALSE) {
    return fail(
        clonecore::ErrorCode::unsupported_layout,
        FAILED(result) ? result : VSS_E_VOLUME_NOT_SUPPORTED,
        L"VSS Volume対応確認",
        L"対象VolumeをVSS Providerがサポートしていません");
  }

  VSS_ID snapshot_id = GUID_NULL;
  result = impl_->components->AddToSnapshotSet(
      const_cast<VSS_PWSZ>(volume_guid_path.c_str()),
      GUID_NULL,
      &snapshot_id);
  if (FAILED(result) || guid_is_null(snapshot_id)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        FAILED(result) ? result : VSS_E_BAD_STATE,
        L"VSS Volume追加",
        L"AddToSnapshotSetが有効なSnapshot IDを返しませんでした");
  }
  impl_->snapshots.push_back(Impl::SnapshotIdentity{
      .original_volume = volume_guid_path,
      .snapshot_id = snapshot_id,
  });
  impl_->step = Impl::Step::volumes_added;
  impl_->log_info(
      L"VSS Volume追加: volume=" + volume_guid_path +
      L", snapshot=" + guid_to_string(snapshot_id));
  return clonecore::success_status();
}

clonecore::Status WindowsVssBackend::prepare_for_backup() {
  const auto ordered = impl_->require_step(
      Impl::Step::volumes_added,
      L"VSS PrepareForBackup順序確認");
  if (!ordered) {
    return ordered;
  }
  if (impl_->snapshots.empty()) {
    return fail(
        clonecore::ErrorCode::invalid_argument,
        E_INVALIDARG,
        L"VSS PrepareForBackup対象確認",
        L"Snapshot対象Volumeがありません");
  }

  ComPtr<IVssAsync> async;
  const HRESULT result =
      impl_->components->PrepareForBackup(async.put());
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS PrepareForBackup開始",
        L"PrepareForBackupに失敗しました");
  }
  const auto waited =
      impl_->wait_async(async.get(), L"VSS PrepareForBackup待機");
  if (!waited) {
    return waited;
  }
  impl_->step = Impl::Step::prepared;
  return clonecore::success_status();
}

clonecore::Status WindowsVssBackend::do_snapshot_set() {
  const auto ordered = impl_->require_step(
      Impl::Step::prepared,
      L"VSS DoSnapshotSet順序確認");
  if (!ordered) {
    return ordered;
  }

  ComPtr<IVssAsync> async;
  const HRESULT result =
      impl_->components->DoSnapshotSet(async.put());
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS DoSnapshotSet開始",
        L"DoSnapshotSetに失敗しました");
  }
  const auto waited =
      impl_->wait_async(async.get(), L"VSS DoSnapshotSet待機");
  if (!waited) {
    return waited;
  }
  impl_->snapshots_created = true;
  impl_->step = Impl::Step::snapshotted;
  return clonecore::success_status();
}

clonecore::Result<std::vector<WriterStatus>>
WindowsVssBackend::query_writer_statuses() {
  const auto ordered = impl_->require_step(
      Impl::Step::snapshotted,
      L"VSS Writer状態取得順序確認");
  if (!ordered) {
    return clonecore::Result<std::vector<WriterStatus>>::failure(
        ordered.error());
  }
  std::vector<WriterStatus> writers;
  const auto status = impl_->collect_writer_statuses(writers, false);
  if (!status) {
    return clonecore::Result<std::vector<WriterStatus>>::failure(
        status.error());
  }
  return clonecore::Result<std::vector<WriterStatus>>::success(
      std::move(writers));
}

clonecore::Result<std::vector<SnapshotMapping>>
WindowsVssBackend::query_snapshot_devices(
    const std::wstring& snapshot_set_id,
    const std::vector<VolumeRequest>& volumes) {
  const auto ordered = impl_->require_step(
      Impl::Step::snapshotted,
      L"VSS Snapshotデバイス取得順序確認");
  if (!ordered) {
    return clonecore::Result<std::vector<SnapshotMapping>>::failure(
        ordered.error());
  }
  if (snapshot_set_id != guid_to_string(impl_->snapshot_set_id) ||
      volumes.size() != impl_->snapshots.size()) {
    return clonecore::Result<std::vector<SnapshotMapping>>::failure(vss_error(
        clonecore::ErrorCode::identity_mismatch,
        E_INVALIDARG,
        L"VSS Snapshot対応Identity確認",
        L"作成したSnapshot setと要求VolumeのIdentityが一致しません"));
  }

  std::vector<SnapshotMapping> mappings;
  mappings.reserve(volumes.size());
  for (const auto& requested : volumes) {
    const auto identity = std::find_if(
        impl_->snapshots.begin(),
        impl_->snapshots.end(),
        [&](const auto& value) {
          return equals_case_insensitive(
              value.original_volume,
              requested.volume_guid_path);
        });
    if (identity == impl_->snapshots.end()) {
      return clonecore::Result<std::vector<SnapshotMapping>>::failure(
          vss_error(
              clonecore::ErrorCode::identity_mismatch,
              VSS_E_OBJECT_NOT_FOUND,
              L"VSS Snapshot対応Volume確認",
              L"要求Volumeに対応するSnapshot IDがありません"));
    }

    SnapshotProperties properties;
    const HRESULT result = impl_->components->GetSnapshotProperties(
        identity->snapshot_id,
        properties.put());
    if (FAILED(result)) {
      return clonecore::Result<std::vector<SnapshotMapping>>::failure(
          vss_error(
              clonecore::ErrorCode::query_failed,
              result,
              L"VSS Snapshot属性取得",
              L"GetSnapshotPropertiesに失敗しました"));
    }
    const auto& value = properties.get();
    const std::wstring original =
        value.m_pwszOriginalVolumeName == nullptr
            ? L""
            : std::wstring(value.m_pwszOriginalVolumeName);
    const std::wstring device =
        value.m_pwszSnapshotDeviceObject == nullptr
            ? L""
            : std::wstring(value.m_pwszSnapshotDeviceObject);
    if (!IsEqualGUID(value.m_SnapshotId, identity->snapshot_id) ||
        !IsEqualGUID(value.m_SnapshotSetId, impl_->snapshot_set_id) ||
        IsEqualGUID(value.m_ProviderId, GUID_NULL) ||
        value.m_tsCreationTimestamp == 0 ||
        !equals_case_insensitive(original, requested.volume_guid_path) ||
        !is_snapshot_device_path(device)) {
      return clonecore::Result<std::vector<SnapshotMapping>>::failure(
          vss_error(
          clonecore::ErrorCode::identity_mismatch,
          VSS_E_OBJECT_NOT_FOUND,
          L"VSS Snapshot属性Identity確認",
          L"VSSが返したSnapshot、provider、creation timestampと固定済みIdentityが一致しません"));
    }
    mappings.push_back(SnapshotMapping{
        .original_volume_guid_path = requested.volume_guid_path,
        .snapshot_id = guid_to_string(value.m_SnapshotId),
        .snapshot_device_path = device,
        .provider_id = guid_to_string(value.m_ProviderId),
        .creation_timestamp = value.m_tsCreationTimestamp,
    });
  }

  impl_->verified_mappings = mappings;
  return clonecore::Result<std::vector<SnapshotMapping>>::success(
      std::move(mappings));
}

clonecore::Status WindowsVssBackend::copy_snapshot_data(
    const std::vector<SnapshotMapping>& mappings) {
  const auto ordered = impl_->require_step(
      Impl::Step::snapshotted,
      L"VSS Snapshotコピー順序確認");
  if (!ordered) {
    return ordered;
  }
  if (mappings.empty() ||
      mappings.size() != impl_->verified_mappings.size()) {
    return fail(
        clonecore::ErrorCode::identity_mismatch,
        E_INVALIDARG,
        L"VSS SnapshotコピーIdentity確認",
        L"検証済みSnapshot対応がありません");
  }

  for (std::size_t index = 0; index < mappings.size(); ++index) {
    const auto& supplied = mappings[index];
    const auto& verified = impl_->verified_mappings[index];
    if (!equals_case_insensitive(
            supplied.original_volume_guid_path,
            verified.original_volume_guid_path) ||
        !equals_case_insensitive(
            supplied.snapshot_id,
            verified.snapshot_id) ||
        !equals_case_insensitive(
            supplied.snapshot_device_path,
            verified.snapshot_device_path) ||
        !equals_case_insensitive(
            supplied.provider_id,
            verified.provider_id) ||
        supplied.creation_timestamp != verified.creation_timestamp ||
        !is_snapshot_device_path(verified.snapshot_device_path)) {
      return fail(
          clonecore::ErrorCode::identity_mismatch,
          E_INVALIDARG,
          L"VSS SnapshotコピーIdentity確認",
          L"コピー要求が検証済みSnapshot対応から変更されています");
    }
  }

  const auto copied = impl_->options.copy_snapshot_data(
      SnapshotCopyContext{
          .snapshot_set_id = guid_to_string(impl_->snapshot_set_id),
          .mappings = impl_->verified_mappings,
      });
  if (!copied) {
    return copied;
  }
  impl_->step = Impl::Step::data_copied;
  return clonecore::success_status();
}

clonecore::Status WindowsVssBackend::backup_complete() {
  const auto ordered = impl_->require_step(
      Impl::Step::data_copied,
      L"VSS BackupComplete順序確認");
  if (!ordered) {
    return ordered;
  }

  ComPtr<IVssAsync> async;
  const HRESULT result =
      impl_->components->BackupComplete(async.put());
  if (FAILED(result)) {
    return fail(
        clonecore::ErrorCode::query_failed,
        result,
        L"VSS BackupComplete開始",
        L"BackupCompleteに失敗しました");
  }
  const auto waited =
      impl_->wait_async(async.get(), L"VSS BackupComplete待機");
  if (!waited) {
    return waited;
  }
  impl_->backup_complete_notified = true;

  std::vector<WriterStatus> final_writers;
  const auto final_status =
      impl_->collect_writer_statuses(final_writers, true);
  if (!final_status) {
    return final_status;
  }
  impl_->step = Impl::Step::backup_completed;
  return clonecore::success_status();
}

clonecore::Status WindowsVssBackend::delete_snapshot_set(
    const std::wstring& snapshot_set_id) {
  if (guid_is_null(impl_->snapshot_set_id) ||
      snapshot_set_id != guid_to_string(impl_->snapshot_set_id)) {
    return fail(
        clonecore::ErrorCode::identity_mismatch,
        E_INVALIDARG,
        L"VSS Snapshot set削除Identity確認",
        L"作成したSnapshot set以外は削除できません");
  }
  return impl_->delete_exact_snapshot_set();
}

}  // namespace ytec::vssrequester

#include "ytec/winpeapp/app_runner.h"
#include "boot_finalization.h"

#include "ytec/bootrepair/offline_windows.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/clonecore/offline_mbr_clone.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/winpeapp/clone_execution_readiness.h"
#include "ytec/winpeapp/product_io_policy.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

clonecore::Error execution_error(
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

class TemporarySourceReadOnly final {
 public:
  TemporarySourceReadOnly(
      clonecore::StableDiskIdentity source,
      const bool restore_required)
      : source_(std::move(source)), restore_required_(restore_required) {}

  ~TemporarySourceReadOnly() {
    if (restore_required_) {
      static_cast<void>(
          diskmodel::set_verified_source_read_only_with_windows_apis(
              source_, false));
    }
  }

  TemporarySourceReadOnly(const TemporarySourceReadOnly&) = delete;
  TemporarySourceReadOnly& operator=(const TemporarySourceReadOnly&) = delete;

  [[nodiscard]] clonecore::Status restore() {
    if (!restore_required_) {
      return clonecore::success_status();
    }
    const auto restored =
        diskmodel::set_verified_source_read_only_with_windows_apis(
            source_, false);
    if (restored) {
      restore_required_ = false;
    }
    return restored;
  }

 private:
  clonecore::StableDiskIdentity source_;
  bool restore_required_{};
};

std::wstring volume_child(
    const std::wstring& root,
    const std::wstring_view relative) {
  std::wstring value = root;
  if (!value.ends_with(L'\\')) {
    value.push_back(L'\\');
  }
  value.append(relative);
  return value;
}

clonecore::Result<bool> detect_supported_windows_source(
    const std::vector<clonecore::VolumeBitmapBinding>& bindings) {
  std::size_t windows_count = 0U;
  for (const auto& binding : bindings) {
    const std::wstring kernel = volume_child(
        binding.volume_device_path, L"Windows\\System32\\ntoskrnl.exe");
    const DWORD attributes = GetFileAttributesW(kernel.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        continue;
      }
      return clonecore::Result<bool>::failure(execution_error(
          clonecore::ErrorCode::query_failed,
          native_code,
          L"PE直接クローンWindows領域検出",
          L"コピー元ボリュームのWindows領域有無を安全に確認できません"));
    }
    const auto verified = bootrepair::verify_offline_windows_amd64(
        binding.volume_device_path);
    if (!verified) {
      return clonecore::Result<bool>::failure(verified.error());
    }
    ++windows_count;
    if (windows_count > 1U) {
      return clonecore::Result<bool>::failure(execution_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DUP_NAME,
          L"PE直接クローンWindows領域検出",
          L"Windows領域が複数あるため、現在の自動起動最終化では対象を一意に確定できません"));
    }
  }
  return clonecore::Result<bool>::success(windows_count == 1U);
}

clonecore::Result<std::vector<std::uint32_t>>
collect_connected_mbr_signatures(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::MbrDisk& source_mbr) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  const auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        execution_error(
            clonecore::ErrorCode::query_failed,
            ERROR_INVALID_DATA,
            L"MBR署名衝突検査の全ディスク列挙",
            L"未解決の列挙診断があるためMBR署名の一意性を確認できません"));
  }

  std::vector<std::uint32_t> signatures;
  signatures.push_back(source_mbr.disk_signature);
  for (const auto& disk : report.value().disks) {
    if (disk.partition_style != diskmodel::PartitionStyle::mbr) {
      continue;
    }
    auto identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          identity.error());
    }
    if (clonecore::validate_stable_identity(
            expected_source, identity.value(), L"コピー元")) {
      continue;
    }
    auto handle =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            identity.value());
    if (!handle) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          handle.error());
    }
    const auto parsed = clonecore::parse_mbr(*handle.value().reader);
    if (!parsed) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          parsed.error());
    }
    signatures.push_back(parsed.value().disk_signature);
  }
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(
      std::unique(signatures.begin(), signatures.end()),
      signatures.end());
  return clonecore::Result<std::vector<std::uint32_t>>::success(
      std::move(signatures));
}

clonecore::Result<CloneExecutionReport> execute_gpt_clone(
    const CloneExecutionRequest& request,
    diskmodel::ReadOnlyPhysicalDiskHandle& source,
    diskmodel::PhysicalTargetHandle& target,
    std::vector<clonecore::VolumeBitmapBinding> bindings) {
  const auto io_policy = select_product_io_policy(
      request.expected_source.logical_sector_size);
  if (!io_policy.has_value()) {
    return clonecore::Result<CloneExecutionReport>::failure(
        execution_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"製品版クローンI/O方針",
            L"実機相当検証までは512バイト論理セクターだけを対象にします"));
  }
  clonecore::WindowsVolumeBitmapProvider bitmap_provider(
      std::move(bindings));
  auto guid_generator = clonecore::make_windows_guid_generator();
  const auto report = clonecore::execute_offline_gpt_clone(
      clonecore::OfflineGptCloneRequest{
          .expected_source = request.expected_source,
          .observed_source = source.observed.identity,
          .expected_target = request.expected_target,
          .observed_target = target.observed.target_identity,
          .confirmation = request.confirmation,
          .maximum_chunk_bytes = io_policy->transfer_chunk_bytes,
          .callbacks = request.callbacks,
      },
      *source.reader,
      *target.target,
      bitmap_provider,
      *guid_generator);
  if (!report) {
    return clonecore::Result<CloneExecutionReport>::failure(
        report.error());
  }
  return clonecore::Result<CloneExecutionReport>::success(
      CloneExecutionReport{
          .partition_style = ClonePartitionStyle::gpt,
          .copied_data_bytes = report.value().copied_data_bytes,
          .copied_partition_count =
              report.value().copied_partition_count,
          .recreated_partition_count =
              report.value().recreated_partition_count,
          .read_back_verified = report.value().read_back_verified,
          .partition_table_committed =
              report.value().primary_gpt_committed,
      });
}

clonecore::Result<CloneExecutionReport> execute_mbr_clone(
    const CloneExecutionRequest& request,
    diskmodel::ReadOnlyPhysicalDiskHandle& source,
    diskmodel::PhysicalTargetHandle& target,
    std::vector<clonecore::VolumeBitmapBinding> bindings,
    std::vector<std::uint32_t> connected_signatures) {
  const auto io_policy = select_product_io_policy(
      request.expected_source.logical_sector_size);
  if (!io_policy.has_value()) {
    return clonecore::Result<CloneExecutionReport>::failure(
        execution_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"製品版クローンI/O方針",
            L"実機相当検証までは512バイト論理セクターだけを対象にします"));
  }
  clonecore::WindowsVolumeBitmapProvider bitmap_provider(
      std::move(bindings));
  auto signature_generator =
      clonecore::make_windows_mbr_signature_generator();
  const auto report = clonecore::execute_offline_mbr_clone(
      clonecore::OfflineMbrCloneRequest{
          .expected_source = request.expected_source,
          .observed_source = source.observed.identity,
          .expected_target = request.expected_target,
          .observed_target = target.observed.target_identity,
          .confirmation = request.confirmation,
          .maximum_chunk_bytes = io_policy->transfer_chunk_bytes,
          .connected_mbr_signatures = std::move(connected_signatures),
          .callbacks = request.callbacks,
      },
      *source.reader,
      *target.target,
      bitmap_provider,
      *signature_generator);
  if (!report) {
    return clonecore::Result<CloneExecutionReport>::failure(
        report.error());
  }
  return clonecore::Result<CloneExecutionReport>::success(
      CloneExecutionReport{
          .partition_style = ClonePartitionStyle::mbr,
          .copied_data_bytes = report.value().copied_data_bytes,
          .copied_partition_count =
              report.value().copied_partition_count,
          .recreated_partition_count = 0,
          .read_back_verified = report.value().read_back_verified,
          .partition_table_committed =
              report.value().target_mbr_committed,
      });
}

class WindowsCloneExecutionService final
    : public ICloneExecutionService {
 public:
  clonecore::Result<CloneExecutionReport> execute(
      const CloneExecutionRequest& request) override {
    if (request.transfer_mode == imageformat::TransferMode::shrink) {
      return clonecore::Result<CloneExecutionReport>::failure(
          execution_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"PE縮小移行モード境界",
              L"単一.tsumugiの縮小payload作成・配置adapterが完全検証されるまで、旧.dcmig経路へ退避せず安全側に停止します"));
    }
    if (request.transfer_mode != imageformat::TransferMode::exact) {
      return clonecore::Result<CloneExecutionReport>::failure(
          execution_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"通常クローンモード境界",
              L"通常モードには縮小移行用の作業イメージを指定できません"));
    }
    if (!request.authorization.empty()) {
      return clonecore::Result<CloneExecutionReport>::failure(
          execution_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_ACCESS_DENIED,
              L"製品版直接クローン許可境界",
              L"製品版ではVM専用許可語を受け付けません"));
    }

    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto initial = diskmodel::reidentify_physical_clone(
        request.expected_source,
        request.expected_target,
        request.confirmation,
        *inventory);
    if (!initial) {
      return clonecore::Result<CloneExecutionReport>::failure(
          initial.error());
    }
    const auto initial_ready = validate_clone_execution_observation(
        initial.value().source, initial.value().target);
    if (!initial_ready) {
      return clonecore::Result<CloneExecutionReport>::failure(
          initial_ready.error());
    }
    const bool restore_source_attribute =
        !initial.value().source.read_only.value();
    if (restore_source_attribute) {
      const auto protected_source =
          diskmodel::set_verified_source_read_only_with_windows_apis(
              request.expected_source, true);
      if (!protected_source) {
        return clonecore::Result<CloneExecutionReport>::failure(
            protected_source.error());
      }
    }
    TemporarySourceReadOnly source_protection(
        request.expected_source, restore_source_attribute);

    auto source =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            request.expected_source);
    if (!source) {
      return clonecore::Result<CloneExecutionReport>::failure(
          source.error());
    }
    const auto reopened_ready = validate_clone_execution_observation(
        source.value().observed.observed, initial.value().target);
    if (!reopened_ready) {
      return clonecore::Result<CloneExecutionReport>::failure(
          reopened_ready.error());
    }

    std::vector<clonecore::VolumeBitmapBinding> bindings;
    std::optional<std::vector<std::uint32_t>> connected_mbr_signatures;
    if (source.value().observed.observed.partition_style ==
        diskmodel::PartitionStyle::gpt) {
      const auto parsed = clonecore::parse_gpt(*source.value().reader);
      if (!parsed) {
        return clonecore::Result<CloneExecutionReport>::failure(
            parsed.error());
      }
      auto queried = diskmodel::query_windows_volume_bitmap_bindings(
          source.value().observed.observed, parsed.value());
      if (!queried) {
        return clonecore::Result<CloneExecutionReport>::failure(
            queried.error());
      }
      bindings = queried.take_value();
    } else if (source.value().observed.observed.partition_style ==
               diskmodel::PartitionStyle::mbr) {
      const auto parsed = clonecore::parse_mbr(*source.value().reader);
      if (!parsed) {
        return clonecore::Result<CloneExecutionReport>::failure(
            parsed.error());
      }
      auto queried = diskmodel::query_windows_volume_bitmap_bindings(
          source.value().observed.observed, parsed.value());
      if (!queried) {
        return clonecore::Result<CloneExecutionReport>::failure(
            queried.error());
      }
      bindings = queried.take_value();
      auto signatures = collect_connected_mbr_signatures(
          request.expected_source, parsed.value());
      if (!signatures) {
        return clonecore::Result<CloneExecutionReport>::failure(
            signatures.error());
      }
      connected_mbr_signatures = signatures.take_value();
    } else {
      return clonecore::Result<CloneExecutionReport>::failure(
          execution_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"製品版クローンのコピー元形式",
              L"GPTまたはMBRコピー元だけを実行できます"));
    }

    const auto supported_windows_source =
        detect_supported_windows_source(bindings);
    if (!supported_windows_source) {
      return clonecore::Result<CloneExecutionReport>::failure(
          supported_windows_source.error());
    }

    const auto offline =
        diskmodel::set_verified_target_offline_with_windows_apis(
            request.expected_source,
            request.expected_target,
            request.confirmation,
            true);
    if (!offline) {
      return clonecore::Result<CloneExecutionReport>::failure(
          offline.error());
    }

    auto target =
        diskmodel::open_verified_physical_target_with_windows_apis(
            request.expected_target, request.confirmation);
    if (!target) {
      return clonecore::Result<CloneExecutionReport>::failure(
          target.error());
    }
    const auto final_ready = validate_clone_execution_observation(
        source.value().observed.observed,
        target.value().observed.target);
    if (!final_ready) {
      return clonecore::Result<CloneExecutionReport>::failure(
          final_ready.error());
    }
    const auto identities = clonecore::validate_clone_identities(
        request.expected_source,
        source.value().observed.identity,
        request.expected_target,
        target.value().observed.target_identity,
        request.confirmation);
    if (!identities) {
      return clonecore::Result<CloneExecutionReport>::failure(
          identities.error());
    }

    auto clone_report =
        source.value().observed.observed.partition_style ==
                diskmodel::PartitionStyle::mbr
            ? execute_mbr_clone(
                  request,
                  source.value(),
                  target.value(),
                  std::move(bindings),
                  std::move(*connected_mbr_signatures))
            : execute_gpt_clone(
                  request,
                  source.value(),
                  target.value(),
                  std::move(bindings));
    if (!clone_report) {
      return clonecore::Result<CloneExecutionReport>::failure(
          clone_report.error());
    }

    // Release the raw target writer before any temporary online transition.
    target.value().target.reset();
    auto report = clone_report.take_value();
    report.boot_finalization_required = supported_windows_source.value();
    const bool online_handoff_required =
        report.boot_finalization_required || !request.leave_target_offline;
    if (online_handoff_required) {
      const auto online =
          diskmodel::set_verified_target_offline_with_windows_apis(
              request.expected_source,
              request.expected_target,
              request.confirmation,
              false);
      if (!online) {
        return clonecore::Result<CloneExecutionReport>::failure(
            online.error());
      }
      report.target_returned_online = true;
    } else {
      report.target_left_offline = true;
    }
    if (!report.boot_finalization_required) {
      report.boot_finalization_verified = true;
      source.value().reader.reset();
      const auto restored_source = source_protection.restore();
      if (!restored_source) {
        return clonecore::Result<CloneExecutionReport>::failure(
            restored_source.error());
      }
      return clonecore::Result<CloneExecutionReport>::success(
          std::move(report));
    }
    const auto finalized = finalize_product_target_boot(
        *inventory,
        request.expected_target,
        report.partition_style == ClonePartitionStyle::gpt
            ? diskmodel::PartitionStyle::gpt
            : diskmodel::PartitionStyle::mbr,
        std::nullopt,
        request.callbacks);
    if (!finalized) {
      const auto protected_offline =
          diskmodel::set_verified_target_offline_with_windows_apis(
              request.expected_source,
              request.expected_target,
              request.confirmation,
              true);
      return clonecore::Result<CloneExecutionReport>::failure(
          protected_offline ? finalized.error() : protected_offline.error());
    }
    report.boot_repair = finalized.value().boot_repair;
    report.windows_partition_temporarily_mounted =
        finalized.value().windows_partition_temporarily_mounted;
    report.system_partition_temporarily_mounted =
        finalized.value().system_partition_temporarily_mounted;
    report.temporary_mounts_released =
        finalized.value().temporary_mounts_released;
    report.boot_finalization_verified =
        finalized.value().final_target_verified;
    if (request.leave_target_offline) {
      const auto final_offline =
          diskmodel::set_verified_target_offline_with_windows_apis(
              request.expected_source,
              request.expected_target,
              request.confirmation,
              true);
      if (!final_offline) {
        return clonecore::Result<CloneExecutionReport>::failure(
            final_offline.error());
      }
      report.target_returned_online = false;
      report.target_left_offline = true;
    }
    source.value().reader.reset();
    const auto restored_source = source_protection.restore();
    if (!restored_source) {
      return clonecore::Result<CloneExecutionReport>::failure(
          restored_source.error());
    }
    return clonecore::Result<CloneExecutionReport>::success(
        std::move(report));
  }
};

}  // namespace

std::unique_ptr<ICloneExecutionService>
make_windows_clone_execution_service() {
  return std::make_unique<WindowsCloneExecutionService>();
}

}  // namespace ytec::winpeapp

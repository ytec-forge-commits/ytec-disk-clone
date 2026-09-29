#pragma once

#include "ytec/windowsapp/adk_management.h"

#include <memory>

namespace ytec::windowsapp {

// Production Windows adapter for the fixed EXE-adjacent managed record,
// CREATE_NEW verified offline layout publication, and exact MSI/MSP managed
// removal. The product controller must validate the closed release gate before
// constructing this adapter.
class WindowsAdkManagementPlatform final : public IAdkManagementPlatform {
 public:
  WindowsAdkManagementPlatform();
  ~WindowsAdkManagementPlatform() override;

  WindowsAdkManagementPlatform(
      const WindowsAdkManagementPlatform&) = delete;
  WindowsAdkManagementPlatform& operator=(
      const WindowsAdkManagementPlatform&) = delete;

  [[nodiscard]] clonecore::Result<
      std::optional<AdkManagedInstallationRecord>>
  load_managed_installation_record() override;

  [[nodiscard]] clonecore::Status
  save_managed_installation_record_create_new(
      const AdkManagedInstallationRecord& record) override;

  [[nodiscard]] clonecore::Status
  remove_managed_installation_record_if_exact(
      const AdkManagedInstallationRecord& record) override;

  [[nodiscard]] clonecore::Status begin_new_offline_layout(
      const std::filesystem::path& layout_root,
      std::string_view manifest_id) override;

  [[nodiscard]] clonecore::Status publish_offline_layout_payload(
      const AdkPinnedPayload& payload,
      const AdkVerifiedPayload& verified_payload) override;

  [[nodiscard]] clonecore::Result<AdkOfflineLayoutReport>
  finalize_offline_layout(const AdkReleaseManifest& manifest) override;

  [[nodiscard]] clonecore::Status abandon_offline_layout() override;

  [[nodiscard]] clonecore::Result<std::vector<std::uint32_t>>
  execute_managed_uninstall(const AdkUninstallPlan& plan) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<IAdkManagementPlatform>
make_windows_adk_management_platform();

}  // namespace ytec::windowsapp

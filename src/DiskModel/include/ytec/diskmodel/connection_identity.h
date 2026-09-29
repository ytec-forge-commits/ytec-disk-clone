#pragma once

#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::diskmodel {

enum class DiskIdentityBindingKind : std::uint8_t {
  persistent_identifier,
  same_connection,
};

struct ConnectionBoundPartitionIdentity final {
  std::uint32_t number{};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  PartitionStyle style{PartitionStyle::unknown};
  std::wstring type;
  std::wstring identifier;
  std::wstring name;
  bool bootable{};
};

// A fallback identity for a fixed USB disk whose bridge exposes no unique
// serial. It is valid only while the original connection remains uninterrupted.
// PhysicalDrive numbers are deliberately excluded because they can change on
// re-enumeration; the SetupAPI interface and location paths bind the same port.
struct ConnectionBoundDiskIdentity final {
  std::wstring device_interface_path;
  std::wstring connection_location_path;
  std::wstring model;
  std::uint64_t size_bytes{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  PartitionStyle partition_style{PartitionStyle::unknown};
  std::wstring disk_identifier;
  std::vector<ConnectionBoundPartitionIdentity> partitions;
};

[[nodiscard]] clonecore::Result<ConnectionBoundDiskIdentity>
make_connection_bound_disk_identity(const DiskInfo& disk);

[[nodiscard]] clonecore::Status validate_connection_bound_disk_identity(
    const ConnectionBoundDiskIdentity& expected,
    const ConnectionBoundDiskIdentity& observed);

// Owns one reviewed disk selection. USB fixed disks without a unique serial
// use the same-connection fallback. Once that connection disappears, changes,
// becomes ambiguous, or is explicitly invalidated, this object is latched into
// reselection_required and an identical reconnect cannot revive the approval.
class DiskSelectionIdentity final {
 public:
  [[nodiscard]] static clonecore::Result<DiskSelectionIdentity> create(
      const DiskInfo& selected);

  [[nodiscard]] DiskIdentityBindingKind kind() const noexcept;
  [[nodiscard]] bool reselection_required() const noexcept;

  void invalidate_connection() noexcept;

  [[nodiscard]] clonecore::Result<DiskInfo> reidentify(
      const InventoryReport& inventory);

 private:
  DiskSelectionIdentity(
      DiskIdentityBindingKind kind,
      std::optional<clonecore::StableDiskIdentity> persistent,
      std::optional<ConnectionBoundDiskIdentity> connection) noexcept;

  DiskIdentityBindingKind kind_{
      DiskIdentityBindingKind::persistent_identifier};
  std::optional<clonecore::StableDiskIdentity> persistent_;
  std::optional<ConnectionBoundDiskIdentity> connection_;
  bool reselection_required_{};
};

}  // namespace ytec::diskmodel

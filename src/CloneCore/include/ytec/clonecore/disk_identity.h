#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ytec::clonecore {

struct StableDiskIdentity final {
  std::uint32_t disk_number{};
  std::wstring model;
  std::uint64_t size_bytes{};
  std::uint32_t logical_sector_size{};
  std::string serial_suffix;
  std::wstring device_instance_id;
  bool is_system_disk{};
};

struct TargetConfirmation final {
  bool first_step_acknowledged{};
  std::wstring typed_token;
};

[[nodiscard]] std::wstring make_target_confirmation_token(
    const StableDiskIdentity& identity);

[[nodiscard]] Status validate_stable_identity(
    const StableDiskIdentity& expected,
    const StableDiskIdentity& observed,
    std::wstring_view role);

[[nodiscard]] Status validate_clone_selection(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& observed_source,
    const StableDiskIdentity& expected_target,
    const StableDiskIdentity& observed_target,
    bool require_target_same_or_larger = true);

[[nodiscard]] Status validate_clone_identities(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& observed_source,
    const StableDiskIdentity& expected_target,
    const StableDiskIdentity& observed_target,
    const TargetConfirmation& confirmation,
    bool require_target_same_or_larger = true);

}  // namespace ytec::clonecore

#pragma once

#include "ytec/diskmodel/disk_inventory.h"

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::clitools {

enum class CliExitCode : int {
  success = 0,
  failure = 1,
  partial_diagnostics = 2,
  invalid_arguments = 64,
};

struct InventoryCliPresentation final {
  std::string_view title;
  std::string_view executable_name;
};

inline constexpr InventoryCliPresentation kDefaultInventoryCliPresentation{
    .title = "Y-TEC ディスク診断（読み取り専用）",
    .executable_name = "ytec-disk-inventory",
};

[[nodiscard]] int run_inventory_cli(
    const std::vector<std::wstring>& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    std::ostream& output,
    std::ostream& error_output,
    InventoryCliPresentation presentation =
        kDefaultInventoryCliPresentation);

}  // namespace ytec::clitools

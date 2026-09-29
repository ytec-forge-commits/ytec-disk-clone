#pragma once

#include "ytec/diskmodel/disk_inventory.h"

#include <string>
#include <string_view>

namespace ytec::diskmodel {

[[nodiscard]] std::string inventory_to_json(const InventoryReport& report);
[[nodiscard]] std::string inventory_to_text(const InventoryReport& report);
[[nodiscard]] std::string json_escape(std::string_view value);
[[nodiscard]] std::string to_utf8(std::wstring_view value);
[[nodiscard]] std::string mask_serial_suffix(std::string_view serial);

}  // namespace ytec::diskmodel


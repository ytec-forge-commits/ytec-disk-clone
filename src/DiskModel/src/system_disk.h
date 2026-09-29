#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <vector>

namespace ytec::diskmodel {

[[nodiscard]] clonecore::Result<std::vector<std::uint32_t>>
query_windows_system_disk_numbers();

}  // namespace ytec::diskmodel

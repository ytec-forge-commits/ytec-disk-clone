#include "ytec/clitools/cli_runner.h"

#include "ytec/clonecore/log.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

int wmain(const int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);

  std::vector<std::wstring> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
  }
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const ytec::clonecore::Logger logger = ytec::clonecore::make_stderr_logger();
  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider(&logger);
  return ytec::clitools::run_inventory_cli(
      arguments,
      *provider,
      std::cout,
      std::cerr,
      ytec::clitools::kDefaultInventoryCliPresentation);
}

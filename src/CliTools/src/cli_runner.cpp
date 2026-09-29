#include "ytec/clitools/cli_runner.h"

#include "ytec/diskmodel/inventory_formatter.h"

#include <algorithm>
#include <ostream>
#include <string_view>

namespace ytec::clitools {
namespace {

enum class OutputFormat {
  text,
  json,
};

void write_usage(
    std::ostream& stream,
    const InventoryCliPresentation presentation) {
  stream << presentation.title << '\n'
         << "使い方: " << presentation.executable_name
         << " [--text | --json] [--help]\n"
            "  --text  人間向けテキストを出力します（既定）\n"
            "  --json  機械可読JSONを標準出力へ出力します\n"
            "  --help  このヘルプを表示します\n"
            "この実行ファイルには書き込み・クローン・復元機能はありません。\n";
}

}  // namespace

int run_inventory_cli(
    const std::vector<std::wstring>& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    std::ostream& output,
    std::ostream& error_output,
    const InventoryCliPresentation presentation) {
  OutputFormat format = OutputFormat::text;
  bool format_was_selected = false;

  for (const std::wstring& argument : arguments) {
    if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
      write_usage(output, presentation);
      return static_cast<int>(CliExitCode::success);
    }
    if (argument == L"--json" || argument == L"--text") {
      const OutputFormat requested =
          argument == L"--json" ? OutputFormat::json : OutputFormat::text;
      if (format_was_selected && requested != format) {
        error_output << "--json と --text は同時に指定できません。\n";
        return static_cast<int>(CliExitCode::invalid_arguments);
      }
      format = requested;
      format_was_selected = true;
      continue;
    }

    error_output << "不明な引数です: " << diskmodel::to_utf8(argument) << '\n';
    write_usage(error_output, presentation);
    return static_cast<int>(CliExitCode::invalid_arguments);
  }

  auto result = provider.enumerate();
  if (!result) {
    const clonecore::Error& failure = result.error();
    error_output << "ディスク列挙を開始できませんでした: "
                 << diskmodel::to_utf8(failure.operation) << " (Windows error "
                 << failure.native_code << ") "
                 << diskmodel::to_utf8(failure.message) << '\n';
    return static_cast<int>(CliExitCode::failure);
  }

  const diskmodel::InventoryReport& report = result.value();
  if (format == OutputFormat::json) {
    output << diskmodel::inventory_to_json(report);
  } else {
    output << diskmodel::inventory_to_text(report);
  }

  return report.issues.empty()
             ? static_cast<int>(CliExitCode::success)
             : static_cast<int>(CliExitCode::partial_diagnostics);
}

}  // namespace ytec::clitools

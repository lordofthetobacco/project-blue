#include "editor/editor_options.h"

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"
#include "editor/commands.h"
#include "editor/editor_settings.h"
#include "platform/window.h"

namespace blue::editor {

core::Result<EditorOptions, std::string> ParseEditorOptions(
    const std::vector<std::string>& args) {
  using ParseResult = core::Result<EditorOptions, std::string>;
  EditorOptions options;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    const bool has_value = i + 1 < args.size();
    if (arg == "--window-pos" || arg == "--window-size") {
      const std::optional<platform::WindowPair> pair =
          has_value ? platform::ParseWindowPair(args[i + 1]) : std::nullopt;
      if (!pair) {
        return ParseResult::Error(std::format("{} expects X,Y", arg));
      }
      if (arg == "--window-pos") {
        options.window_pos = pair;
      } else if (pair->x > 0 && pair->y > 0) {
        options.window_size = *pair;
      } else {
        return ParseResult::Error("--window-size must be positive");
      }
      ++i;
    } else if (arg == "--exec") {
      if (!has_value) {
        return ParseResult::Error("--exec expects a command line");
      }
      options.exec = args[++i];
    } else if (arg == "--mcp") {
      options.mcp = true;
    } else if (arg == "--mcp-port") {
      core::Result<int, std::string> port =
          has_value ? ParseIntArg(args[i + 1], "--mcp-port")
                    : core::Result<int, std::string>::Error(
                          "--mcp-port expects a port");
      if (!port.ok() || port.value() < kMinMcpPort ||
          port.value() > kMaxMcpPort) {
        return ParseResult::Error(std::format(
            "--mcp-port expects a port in {}..{}", kMinMcpPort, kMaxMcpPort));
      }
      options.mcp = true;
      options.mcp_port = port.value();
      ++i;
    } else if (arg.starts_with("--")) {
      return ParseResult::Error(std::format("unknown option {}", arg));
    } else if (options.file.empty()) {
      options.file = arg;
    } else {
      return ParseResult::Error(std::format("unexpected argument {}", arg));
    }
  }
  return ParseResult::Ok(std::move(options));
}

}  // namespace blue::editor

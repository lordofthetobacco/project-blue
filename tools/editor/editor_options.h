#ifndef BLUE_EDITOR_EDITOR_OPTIONS_H_
#define BLUE_EDITOR_EDITOR_OPTIONS_H_

#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "platform/window.h"
#include "render/backend.h"

namespace blue::editor {

// Command line of blue-editor:
//   blue-editor [file.pbo|file.mscn] [--window-pos X,Y] [--window-size W,H]
//               [--exec "<commands>"] [--mcp] [--mcp-port N]
//               [--renderer=vulkan|d3d12|metal]
// --mcp starts the MCP server (Edit > Settings...) with the saved port,
// --mcp-port N starts it on port N; neither is saved in the settings.
struct EditorOptions {
  std::string file;  // Opened at startup when not empty.
  std::optional<platform::WindowPair> window_pos;
  platform::WindowPair window_size{1600, 900};
  std::string exec;  // Commands queued after the first frame.
  bool mcp = false;  // --mcp or --mcp-port
  std::optional<int> mcp_port;
  // The value of --renderer=, unchecked; main() resolves it (with
  // BLUE_RENDERER) into `backend` through render::ChooseBackend().
  std::optional<std::string> renderer;
  render::Backend backend = render::DefaultBackend();
};

// Parses the arguments after the program name; the error is readable.
[[nodiscard]] core::Result<EditorOptions, std::string> ParseEditorOptions(
    const std::vector<std::string>& args);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_EDITOR_OPTIONS_H_

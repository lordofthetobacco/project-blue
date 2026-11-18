// blue-editor: the Prefab / Scene editor. See editor/editor_app.h for the
// command line.

#include <SDL3/SDL_main.h>

#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/result.h"
#include "editor/editor_app.h"
#include "editor/modes.h"

int main(int argc, char* argv[]) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  blue::core::Result<blue::editor::EditorOptions, std::string> options =
      blue::editor::ParseEditorOptions(args);
  if (!options.ok()) {
    blue::core::log::Error(
        "{}\nusage: blue-editor [file.pbo|file.mscn] [--window-pos X,Y] "
        "[--window-size W,H] [--exec \"<commands>\"] [--mcp] [--mcp-port N]",
        options.error());
    return 2;
  }
  blue::editor::EditorApp app(std::move(options).value());
  blue::editor::RegisterPrefabMode(app);
  blue::editor::RegisterSceneMode(app);
  return app.Run();
}

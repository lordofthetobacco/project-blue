// blue-editor: the Prefab / Scene editor. See editor/editor_app.h for the
// command line.

#include <SDL3/SDL_main.h>
#include <SDL3/SDL_stdinc.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/result.h"
#include "editor/editor_app.h"
#include "editor/modes.h"
#include "render/backend.h"

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
        "[--window-size W,H] [--exec \"<commands>\"] [--mcp] [--mcp-port N] "
        "[--renderer=vulkan|d3d12|metal]",
        options.error());
    return 2;
  }
  const char* environment = SDL_getenv("BLUE_RENDERER");
  const auto backend = blue::render::ChooseBackend(
      options.value().renderer,
      environment != nullptr ? std::optional<std::string_view>(environment)
                             : std::nullopt);
  if (!backend.ok()) {
    blue::core::log::Error("{}", backend.error());
    return 2;
  }
  options.value().backend = backend.value();
  blue::editor::EditorApp app(std::move(options).value());
  blue::editor::RegisterPrefabMode(app);
  blue::editor::RegisterSceneMode(app);
  return app.Run();
}

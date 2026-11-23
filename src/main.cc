#include <SDL3/SDL_main.h>
#include <SDL3/SDL_stdinc.h>

#include <chrono>
#include <optional>
#include <string_view>

#include "app.h"
#include "core/log.h"
#include "render/backend.h"

int main(int argc, char* argv[]) {
  const auto process_start = std::chrono::steady_clock::now();
  const auto options = blue::ParseCommandLine(argc, argv);
  if (!options) {
    blue::core::log::Error("{}", blue::kUsage);
    return 2;
  }

  if (options->show_help) {
    blue::core::log::Info("{}", blue::kUsage);
    return 0;
  }

  const char* environment = SDL_getenv("BLUE_RENDERER");
  const auto backend = blue::render::ChooseBackend(
      options->renderer,
      environment != nullptr ? std::optional<std::string_view>(environment)
                             : std::nullopt);
  if (!backend.ok()) {
    blue::core::log::Error("{}", backend.error());
    return 2;
  }

  blue::App app(*options, backend.value(), process_start);
  return app.Run();
}

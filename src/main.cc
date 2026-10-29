#include <SDL3/SDL_main.h>

#include "app.h"
#include "core/log.h"

int main(int argc, char* argv[]) {
  const auto options = blue::ParseCommandLine(argc, argv);
  if (!options) {
    blue::core::log::Error("{}", blue::kUsage);
    return 2;
  }

  if (options->show_help) {
    blue::core::log::Info("{}", blue::kUsage);
    return 0;
  }

  blue::App app(*options);
  return app.Run();
}

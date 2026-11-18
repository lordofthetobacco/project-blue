#include <SDL3/SDL_main.h>

#include <chrono>

#include "app.h"
#include "core/log.h"

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

  blue::App app(*options, process_start);
  return app.Run();
}

#include "App.hpp"

#include "core/Log.hpp"

#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
    const auto options = blue::parseCommandLine(argc, argv);
    if (!options) {
        blue::log::error("{}", blue::kUsage);
        return 2;
    }

    if (options->showHelp) {
        blue::log::info("{}", blue::kUsage);
        return 0;
    }

    blue::App app(*options);
    return app.run();
}

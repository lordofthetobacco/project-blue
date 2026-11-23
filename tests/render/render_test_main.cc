// blue_render_tests: headless GPU render tests (design §5). Renders fixed
// scenes without a window and compares them with tests/render/golden.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tests/render/harness_options.h"
#include "tests/render/png_io.h"
#include "tests/render/scenes.h"

namespace blue::render_test {
namespace {

constexpr std::array<std::string_view, 1> kCompiledBackends = {"vulkan"};

HarnessOptions& MutableOptions() {
  static HarnessOptions* options = new HarnessOptions();
  return *options;
}

// Returns the value of `arg` if it is `--<name>=<value>`.
std::optional<std::string> FlagValue(std::string_view arg,
                                     std::string_view name) {
  const std::string prefix = "--" + std::string(name) + "=";
  if (!arg.starts_with(prefix)) {
    return std::nullopt;
  }
  return std::string(arg.substr(prefix.size()));
}

// Tiles every golden image into `path`; returns false on failure.
bool WriteContactSheet(const std::string& path) {
  std::vector<LabeledImage> tiles;
  for (const std::string_view name : SceneNames()) {
    std::optional<RgbaImage> golden =
        ReadPng(std::string(BLUE_RENDER_GOLDEN_DIR) + "/" + std::string(name) +
                ".png");
    if (!golden) {
      std::fprintf(stderr, "contact sheet: no golden for %.*s\n",
                   static_cast<int>(name.size()), name.data());
      continue;
    }
    tiles.push_back({.label = std::string(name), .image = std::move(*golden)});
  }
  if (tiles.empty()) {
    return false;
  }
  return WritePng(path, MakeContactSheet(tiles, 3));
}

}  // namespace

std::span<const std::string_view> CompiledBackends() {
  return kCompiledBackends;
}

std::optional<HarnessOptions> ParseHarnessOptions(
    std::vector<std::string>& args, std::string default_out_dir) {
  HarnessOptions options;
  options.out_dir = std::move(default_out_dir);
  std::vector<std::string> rest;
  for (const std::string& arg : args) {
    if (std::optional<std::string> backend = FlagValue(arg, "backend")) {
      if (std::ranges::find(kCompiledBackends, *backend) ==
          kCompiledBackends.end()) {
        std::fprintf(stderr, "Backend '%s' is not compiled in (have: vulkan)\n",
                     backend->c_str());
        return std::nullopt;
      }
      options.backends.push_back(*backend);
    } else if (std::optional<std::string> adapter = FlagValue(arg, "adapter")) {
      if (*adapter == "hw") {
        options.adapter = render::AdapterPreference::kHighPerformance;
      } else if (*adapter == "software") {
        options.adapter = render::AdapterPreference::kSoftware;
      } else {
        std::fprintf(stderr, "--adapter must be hw or software, not '%s'\n",
                     adapter->c_str());
        return std::nullopt;
      }
    } else if (arg == "--update-golden") {
      options.update_golden = true;
    } else if (std::optional<std::string> out = FlagValue(arg, "out")) {
      options.out_dir = *out;
    } else if (std::optional<std::string> sheet =
                   FlagValue(arg, "contact-sheet")) {
      options.contact_sheet = *sheet;
    } else {
      rest.push_back(arg);
    }
  }
  if (options.backends.empty()) {
    options.backends.assign(kCompiledBackends.begin(), kCompiledBackends.end());
  }
  args = std::move(rest);
  return options;
}

const HarnessOptions& Options() { return MutableOptions(); }

void SetOptions(HarnessOptions options) {
  MutableOptions() = std::move(options);
}

}  // namespace blue::render_test

int main(int argc, char** argv) {
  using blue::render_test::HarnessOptions;
  std::vector<std::string> args(argv + 1, argv + argc);
  std::optional<HarnessOptions> options =
      blue::render_test::ParseHarnessOptions(args, BLUE_RENDER_TEST_OUT_DIR);
  if (!options) {
    return 2;
  }
  const std::string contact_sheet = options->contact_sheet;
  blue::render_test::SetOptions(std::move(*options));

  std::vector<char*> doctest_argv{argv[0]};
  for (std::string& arg : args) {
    doctest_argv.push_back(arg.data());
  }
  doctest::Context context(static_cast<int>(doctest_argv.size()),
                           doctest_argv.data());
  const int result = context.run();
  if (context.shouldExit()) {
    return result;
  }
  if (!contact_sheet.empty()) {
    if (!blue::render_test::WriteContactSheet(contact_sheet)) {
      std::fprintf(stderr, "Could not write the contact sheet %s\n",
                   contact_sheet.c_str());
      return result == 0 ? 1 : result;
    }
    std::printf("Contact sheet: %s\n", contact_sheet.c_str());
  }
  return result;
}

#include "render/backend.h"

#include <doctest/doctest.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "platform/window_graphics.h"

namespace blue::render {

TEST_CASE("Backend names round-trip") {
  for (const Backend backend :
       {Backend::kVulkan, Backend::kD3d12, Backend::kMetal}) {
    CHECK(ParseBackend(BackendName(backend)) == backend);
  }
  CHECK(BackendName(Backend::kVulkan) == "vulkan");
  CHECK(BackendName(Backend::kD3d12) == "d3d12");
  CHECK(BackendName(Backend::kMetal) == "metal");
  CHECK_FALSE(ParseBackend("Vulkan"));
  CHECK_FALSE(ParseBackend(""));
  CHECK_FALSE(ParseBackend("opengl"));
}

TEST_CASE("Each backend gets its window graphics") {
  CHECK(WindowGraphicsFor(Backend::kVulkan) ==
        platform::WindowGraphics::kVulkan);
  CHECK(WindowGraphicsFor(Backend::kD3d12) == platform::WindowGraphics::kNone);
  CHECK(WindowGraphicsFor(Backend::kMetal) == platform::WindowGraphics::kMetal);
}

TEST_CASE("The compiled backends include the default") {
  const std::span<const Backend> compiled = CompiledBackends();
  REQUIRE_FALSE(compiled.empty());
  CHECK(IsCompiled(DefaultBackend()));
  for (const Backend backend : compiled) {
    CHECK(IsCompiled(backend));
  }
}

TEST_CASE("ChooseBackend prefers the command line, then the environment") {
  const Backend fallback = DefaultBackend();
  const std::string_view name = BackendName(fallback);

  SUBCASE("Nothing set: the default") {
    const auto chosen = ChooseBackend(std::nullopt, std::nullopt);
    REQUIRE(chosen.ok());
    CHECK(chosen.value() == fallback);
  }
  SUBCASE("An empty environment variable counts as unset") {
    const auto chosen = ChooseBackend(std::nullopt, std::string_view());
    REQUIRE(chosen.ok());
    CHECK(chosen.value() == fallback);
  }
  SUBCASE("The environment names a compiled backend") {
    const auto chosen = ChooseBackend(std::nullopt, name);
    REQUIRE(chosen.ok());
    CHECK(chosen.value() == fallback);
  }
  SUBCASE("The command line wins over a bad environment value") {
    const auto chosen = ChooseBackend(name, std::string_view("nonsense"));
    REQUIRE(chosen.ok());
    CHECK(chosen.value() == fallback);
  }
  SUBCASE("An unknown name is an error listing the compiled backends") {
    const auto chosen = ChooseBackend(std::string_view("opengl"), std::nullopt);
    REQUIRE_FALSE(chosen.ok());
    CHECK(chosen.error().find("--renderer") != std::string::npos);
    CHECK(chosen.error().find("opengl") != std::string::npos);
    CHECK(chosen.error().find(name) != std::string::npos);
  }
  SUBCASE("A known backend that is not compiled in is an error") {
    for (const Backend backend :
         {Backend::kVulkan, Backend::kD3d12, Backend::kMetal}) {
      if (IsCompiled(backend)) {
        continue;
      }
      const auto chosen = ChooseBackend(std::nullopt, BackendName(backend));
      REQUIRE_FALSE(chosen.ok());
      CHECK(chosen.error().find("BLUE_RENDERER") != std::string::npos);
      CHECK(chosen.error().find("not compiled") != std::string::npos);
    }
  }
}

}  // namespace blue::render

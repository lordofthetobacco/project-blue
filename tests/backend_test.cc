#include "render/backend.h"

#include <doctest/doctest.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("FallbackOrder: requested, default, then the compiled order") {
  using enum Backend;
  const std::vector<Backend> all = {kVulkan, kD3d12, kMetal};

  SUBCASE("Windows default d3d12, nothing requested beyond it") {
    const std::vector<Backend> compiled = {kVulkan, kD3d12};
    CHECK(FallbackOrder(kD3d12, kD3d12, compiled) ==
          std::vector<Backend>{kD3d12, kVulkan});
  }
  SUBCASE("An explicit request falls back too, to the default first") {
    CHECK(FallbackOrder(kMetal, kD3d12, all) ==
          std::vector<Backend>{kMetal, kD3d12, kVulkan});
    CHECK(
        FallbackOrder(kVulkan, kD3d12, std::vector<Backend>{kVulkan, kD3d12}) ==
        std::vector<Backend>{kVulkan, kD3d12});
  }
  SUBCASE("Each backend once, only compiled ones") {
    const std::vector<Backend> compiled = {kVulkan};
    CHECK(FallbackOrder(kD3d12, kMetal, compiled) ==
          std::vector<Backend>{kVulkan});
    CHECK(FallbackOrder(kVulkan, kVulkan, compiled) ==
          std::vector<Backend>{kVulkan});
  }
  SUBCASE("Apple: default metal, then Vulkan (MoltenVK)") {
    const std::vector<Backend> compiled = {kVulkan, kMetal};
    CHECK(FallbackOrder(kMetal, kMetal, compiled) ==
          std::vector<Backend>{kMetal, kVulkan});
  }
  SUBCASE("This build: starts with the default, covers every backend") {
    const std::span<const Backend> compiled = CompiledBackends();
    const std::vector<Backend> order =
        FallbackOrder(DefaultBackend(), DefaultBackend(), compiled);
    REQUIRE_FALSE(order.empty());
    CHECK(order.front() == DefaultBackend());
    CHECK(order.size() == compiled.size());
  }
}

TEST_CASE("Each fallback step needs its own window graphics") {
  // D3D12 presents to a plain window, Vulkan to an SDL_WINDOW_VULKAN one:
  // the fallback between them must re-create the window.
  CHECK(WindowGraphicsFor(Backend::kD3d12) !=
        WindowGraphicsFor(Backend::kVulkan));
  CHECK(WindowGraphicsFor(Backend::kMetal) !=
        WindowGraphicsFor(Backend::kVulkan));
}

}  // namespace blue::render

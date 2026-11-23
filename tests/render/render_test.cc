#include <SDL3/SDL_init.h>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "render/renderer.h"
#include "render/vulkan_context.h"
#include "shader/shader_compiler.h"
#include "tests/render/harness_options.h"
#include "tests/render/image_compare.h"
#include "tests/render/png_io.h"
#include "tests/render/scenes.h"

namespace blue::render_test {
namespace {

// What rendering one scene produced.
struct SceneResult {
  RgbaImage image;
  render::VulkanContext::ValidationCounts validation;
  bool validation_enabled = false;
};

// A fresh headless renderer for every scene: no state leaks between scenes,
// so each golden depends only on its own scene.
std::unique_ptr<render::Renderer> MakeRenderer() {
  return std::make_unique<render::Renderer>(render::RendererOptions{
      .window = nullptr,
      .shader_dir = BLUE_SHADER_SOURCE_DIR,
      // Fixed (not per build type): Debug and Release must render the same.
      .shader_mode = shader::CompileMode::kRelease,
      .adapter = Options().adapter,
      .headless_extent = {kSceneWidth, kSceneHeight},
      .max_anisotropy = 1.0f,
  });
}

// Draws `scene_name` and reads back its last frame (alpha forced to 255).
SceneResult RenderScene(std::string_view scene_name) {
  std::unique_ptr<render::Renderer> renderer = MakeRenderer();
  const std::unique_ptr<TestScene> scene = BuildScene(scene_name, *renderer);
  uint64_t ticket = 0;
  for (int frame = 0; frame < scene->frames; ++frame) {
    const bool last = frame + 1 == scene->frames;
    if (scene->viewport_output) {
      const render::ViewportImage viewport =
          renderer->PrepareViewport({kSceneWidth, kSceneHeight});
      REQUIRE(viewport.extent.width == kSceneWidth);
      if (last) {
        ticket = renderer->RequestViewportReadback();
      }
      renderer->DrawViewportFrame(scene->view, nullptr);
    } else {
      if (last) {
        ticket = renderer->RequestOutputReadback();
      }
      renderer->DrawFrame(scene->view, nullptr);
    }
  }
  renderer->WaitIdle();

  render::ViewportPixels pixels;
  REQUIRE(renderer->TakeViewportReadback(ticket, pixels) ==
          render::ReadbackStatus::kReady);
  CHECK(renderer->TakeViewportReadback(ticket, pixels) ==
        render::ReadbackStatus::kUnknown);

  SceneResult result;
  result.image.width = pixels.width;
  result.image.height = pixels.height;
  result.image.rgba.resize(pixels.rgba.size());
  for (size_t i = 0; i < pixels.rgba.size(); ++i) {
    result.image.rgba[i] = (i % 4 == 3) ? uint8_t{255}
                                        : static_cast<uint8_t>(pixels.rgba[i]);
  }
  result.validation_enabled = renderer->context().validation();
  result.validation = renderer->context().validation_counts();
  return result;
}

std::string GoldenPath(std::string_view scene) {
  return std::string(BLUE_RENDER_GOLDEN_DIR) + "/" + std::string(scene) +
         ".png";
}

std::string OutPath(std::string_view scene, std::string_view suffix) {
  return Options().out_dir + "/" + std::string(scene) + "." +
         std::string(suffix) + ".png";
}

// The tolerance against the goldens: exact on the reference hardware
// (design §5.3), the software tolerance on WARP / lavapipe.
const Tolerance& GoldenTolerance() {
  return Options().adapter == render::AdapterPreference::kSoftware
             ? kSoftwareAdapter
             : kExact;
}

TEST_CASE("Headless scenes match the golden images") {
  for (const std::string& backend : Options().backends) {
    for (const std::string_view scene : SceneNames()) {
      const std::string label = std::string(scene) + "." + backend;
      INFO("scene ", label);
      const auto start = std::chrono::steady_clock::now();
      const SceneResult result = RenderScene(scene);
      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
      REQUIRE(result.image.valid());
      CHECK(result.image.width == kSceneWidth);
      CHECK(result.image.height == kSceneHeight);
      CHECK_MESSAGE(result.validation.errors == 0,
                    "Vulkan validation errors: ", result.validation.errors);
      CHECK(WritePng(OutPath(scene, backend), result.image));

      if (Options().update_golden) {
        REQUIRE(WritePng(GoldenPath(scene), result.image));
        std::printf("%-18s %s  golden written (%.2f s, validation %s, %u "
                    "warnings)\n",
                    std::string(scene).c_str(), backend.c_str(), seconds,
                    result.validation_enabled ? "on" : "off",
                    result.validation.warnings);
        continue;
      }
      const std::optional<RgbaImage> golden = ReadPng(GoldenPath(scene));
      REQUIRE_MESSAGE(golden.has_value(), "missing golden ",
                      GoldenPath(scene));
      const Tolerance& tolerance = GoldenTolerance();
      const CompareMetrics metrics =
          Compare(result.image, *golden, tolerance.outlier_threshold);
      const bool passed = Passes(metrics, tolerance);
      std::printf("%-18s %s  %s  %s (%.2f s, validation %s, %u warnings)\n",
                  std::string(scene).c_str(), backend.c_str(),
                  passed ? "ok  " : "FAIL", Describe(metrics).c_str(), seconds,
                  result.validation_enabled ? "on" : "off",
                  result.validation.warnings);
      if (!passed) {
        CHECK(WritePng(OutPath(scene, "ref"), *golden));
        if (metrics.same_size) {
          CHECK(WritePng(OutPath(scene, "diff"),
                         AmplifiedDiff(result.image, *golden, 16)));
        }
      }
      CHECK_MESSAGE(passed, label, " vs golden (", tolerance.name,
                    "): ", Describe(metrics));
    }
  }
}

TEST_CASE("Rendering the same scene twice gives identical images") {
  const SceneResult first = RenderScene("lit_full_aces");
  const SceneResult second = RenderScene("lit_full_aces");
  const CompareMetrics metrics = Compare(first.image, second.image, 0);
  CHECK_MESSAGE(Passes(metrics, kExact), Describe(metrics));
}

TEST_CASE("Headless rendering never starts SDL video") {
  // Runs after the scenes (doctest keeps file order): nothing in them may
  // have initialized the video subsystem.
  { const SceneResult result = RenderScene("clear_and_upload"); }
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
}

}  // namespace
}  // namespace blue::render_test

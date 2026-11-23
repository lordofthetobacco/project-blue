#include <SDL3/SDL_init.h>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "platform/file_system.h"
#include "render/backend.h"
#include "render/renderer.h"
#include "shader/compile_mode.h"
#include "tests/render/harness_options.h"
#include "tests/render/image_compare.h"
#include "tests/render/png_io.h"
#include "tests/render/scenes.h"
#include "tests/scratch_directory.h"

namespace blue::render_test {
namespace {

// What rendering one scene produced.
struct SceneResult {
  RgbaImage image;
  render::ValidationStats validation;
};

// A fresh headless renderer for every scene: no state leaks between scenes,
// so each golden depends only on its own scene.
std::unique_ptr<render::Renderer> MakeRenderer(render::Backend backend) {
  auto renderer = std::make_unique<render::Renderer>(render::RendererOptions{
      .window = nullptr,
      .backend = backend,
      .shader_dir = BLUE_SHADER_BLOB_DIR,
      // The build's blobs are optimized in every build type, so Debug and
      // Release render the same; no source directory: no hot reload.
      .adapter = Options().adapter,
      .headless_extent = {kSceneWidth, kSceneHeight},
      .max_anisotropy = 1.0f,
  });
  // A fallback to another backend would compare the wrong renderer.
  REQUIRE(renderer->backend() == backend);
  return renderer;
}

// Draws `scene` with `renderer` and reads back its last frame (alpha forced
// to 255).
SceneResult DrawScene(render::Renderer* renderer, const TestScene* scene) {
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
  result.validation = renderer->validation_stats();
  return result;
}

// Draws `scene_name` with a fresh renderer.
SceneResult RenderScene(std::string_view scene_name, render::Backend backend) {
  const std::unique_ptr<render::Renderer> renderer = MakeRenderer(backend);
  const std::unique_ptr<TestScene> scene = BuildScene(scene_name, *renderer);
  return DrawScene(renderer.get(), scene.get());
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
  for (const render::Backend backend_id : Options().backends) {
    const std::string backend(render::BackendName(backend_id));
    for (const std::string_view scene : SceneNames()) {
      const std::string label = std::string(scene) + "." + backend;
      INFO("scene ", label);
      const auto start = std::chrono::steady_clock::now();
      const SceneResult result = RenderScene(scene, backend_id);
      const double seconds = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
      REQUIRE(result.image.valid());
      CHECK(result.image.width == kSceneWidth);
      CHECK(result.image.height == kSceneHeight);
      CHECK_MESSAGE(result.validation.errors == 0,
                    "GPU validation errors: ", result.validation.errors);
      CHECK(WritePng(OutPath(scene, backend), result.image));

      if (Options().update_golden) {
        REQUIRE(WritePng(GoldenPath(scene), result.image));
        std::printf("%-18s %s  golden written (%.2f s, validation %s, %u "
                    "warnings)\n",
                    std::string(scene).c_str(), backend.c_str(), seconds,
                    result.validation.enabled ? "on" : "off",
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
                  result.validation.enabled ? "on" : "off",
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
  for (const render::Backend backend : Options().backends) {
    INFO("backend ", render::BackendName(backend));
    const SceneResult first = RenderScene("lit_full_aces", backend);
    const SceneResult second = RenderScene("lit_full_aces", backend);
    const CompareMetrics metrics = Compare(first.image, second.image, 0);
    CHECK_MESSAGE(Passes(metrics, kExact), Describe(metrics));
  }
}

#if BLUE_SHADER_HOT_RELOAD
// Copies the shader sources into `dir`.
void CopyShaderSources(const testing::ScratchDirectory& dir) {
  const auto names = platform::ListDirectory(BLUE_SHADER_SOURCE_DIR);
  REQUIRE(names.has_value());
  for (const std::string& name : *names) {
    if (!name.ends_with(".slang") && !name.ends_with(".h")) {
      continue;
    }
    const auto bytes = platform::ReadFile(
        std::string(BLUE_SHADER_SOURCE_DIR) + "/" + name, size_t{1} << 20);
    REQUIRE(bytes.ok());
    REQUIRE(platform::WriteFileAtomic(dir.Child(name), bytes.value()).ok());
  }
}

// Returns the text of shader `name` in `dir`.
std::string ReadShader(const testing::ScratchDirectory& dir,
                       std::string_view name) {
  const auto bytes = platform::ReadFile(dir.Child(name), size_t{1} << 20);
  REQUIRE(bytes.ok());
  return std::string(reinterpret_cast<const char*>(bytes.value().data()),
                     bytes.value().size());
}

void WriteShader(const testing::ScratchDirectory& dir, std::string_view name,
                 std::string_view text) {
  REQUIRE(platform::WriteFileAtomic(
              dir.Child(name),
              std::as_bytes(std::span(text.data(), text.size())))
              .ok());
}

// The watcher reports an edit once it is stable across two polls.
void PollReloads(render::Renderer* renderer) {
  for (int i = 0; i < 3; ++i) {
    renderer->ReloadChangedShaders();
  }
}

TEST_CASE("An edited shader is reloaded; a broken edit keeps the old one") {
  constexpr std::string_view kOutput = "return float4(encoded, luma);";
  for (const render::Backend backend : Options().backends) {
    if (backend != render::Backend::kVulkan) {
      continue;  // The Metal backend has its own shader sources.
    }
    // A copy of the sources: the test edits them.
    const testing::ScratchDirectory dir("hot_reload");
    CopyShaderSources(dir);
    const std::string original = ReadShader(dir, "tonemap.slang");
    const size_t at = original.find(kOutput);
    REQUIRE(at != std::string::npos);

    auto renderer = std::make_unique<render::Renderer>(render::RendererOptions{
        .window = nullptr,
        .backend = backend,
        .shader_dir = BLUE_SHADER_BLOB_DIR,
        .shader_source_dir = dir.path(),
        .shader_mode = shader::CompileMode::kRelease,
        .adapter = Options().adapter,
        .headless_extent = {kSceneWidth, kSceneHeight},
        .max_anisotropy = 1.0f,
    });
    REQUIRE(renderer->backend() == backend);
    const std::unique_ptr<TestScene> scene = BuildScene("lit_basic", *renderer);
    const RgbaImage before = DrawScene(renderer.get(), scene.get()).image;

    // Nothing changed: nothing reloads.
    PollReloads(renderer.get());
    CHECK(renderer->last_shader_error().empty());
    CHECK(Matches(DrawScene(renderer.get(), scene.get()).image, before, kExact));

    // Swap red and blue in the tonemapper's output. Everything after it
    // (FXAA by luma, the sRGB decode) treats the channels alike, so the
    // image is the old one with the channels swapped, exactly.
    std::string swapped = original;
    swapped.replace(at, kOutput.size(), "return float4(encoded.bgr, luma);");
    WriteShader(dir, "tonemap.slang", swapped);
    PollReloads(renderer.get());
    CHECK_MESSAGE(renderer->last_shader_error().empty(),
                  renderer->last_shader_error());
    const RgbaImage edited = DrawScene(renderer.get(), scene.get()).image;
    RgbaImage expected = before;
    for (size_t i = 0; i + 3 < expected.rgba.size(); i += 4) {
      std::swap(expected.rgba[i], expected.rgba[i + 2]);
    }
    CHECK_FALSE(Matches(edited, before, kExact));
    const CompareMetrics metrics = Compare(edited, expected, 0);
    CHECK_MESSAGE(Passes(metrics, kExact), Describe(metrics));

    // A broken edit: the error is reported and the pipeline stays.
    std::string broken = original;
    broken.replace(at, kOutput.size(), "return float4(encoded, luma;");
    WriteShader(dir, "tonemap.slang", broken);
    PollReloads(renderer.get());
    const std::string error(renderer->last_shader_error());
    CHECK(error.find("tonemap.slang") != std::string::npos);
    CHECK(Matches(DrawScene(renderer.get(), scene.get()).image, edited, kExact));

    // Fixed: back to the first image, the error cleared.
    WriteShader(dir, "tonemap.slang", original);
    PollReloads(renderer.get());
    CHECK(renderer->last_shader_error().empty());
    const SceneResult fixed = DrawScene(renderer.get(), scene.get());
    CHECK(Matches(fixed.image, before, kExact));
    CHECK(fixed.validation.errors == 0);

    // An edit to a shared file reloads the programs that include it.
    std::string sanitize = ReadShader(dir, "sanitize.slang");
    constexpr std::string_view kLimit = "kMaxSceneRadiance = 4096.0;";
    const size_t limit_at = sanitize.find(kLimit);
    REQUIRE(limit_at != std::string::npos);
    sanitize.replace(limit_at, kLimit.size(), "kMaxSceneRadiance = 0.25;");
    WriteShader(dir, "sanitize.slang", sanitize);
    PollReloads(renderer.get());
    CHECK_MESSAGE(renderer->last_shader_error().empty(),
                  renderer->last_shader_error());
    CHECK_FALSE(
        Matches(DrawScene(renderer.get(), scene.get()).image, before, kExact));
  }
}
#endif  // BLUE_SHADER_HOT_RELOAD

TEST_CASE("Headless rendering never starts SDL video") {
  // Runs after the scenes (doctest keeps file order): nothing in them may
  // have initialized the video subsystem.
  for (const render::Backend backend : Options().backends) {
    const SceneResult result = RenderScene("clear_and_upload", backend);
  }
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
}

}  // namespace
}  // namespace blue::render_test

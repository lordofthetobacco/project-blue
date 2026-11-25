// An offscreen preview of the cooked "Robot Kyle" figures holding their
// weapons, through the real renderer and world (the game's draw path). Not
// a golden test: skipped unless asked for, it needs the cooked sample assets
// (blue_sample_assets) and writes PNGs for a person to look at:
//
//   blue_render_tests -tc="*Kyle*" --no-skip --out=<dir>

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "asset/image_decoder.h"
#include "camera/projection.h"
#include "core/path.h"
#include "render/backend.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "scene/default_lighting.h"
#include "scene/scene_data.h"
#include "tests/render/harness_options.h"
#include "tests/render/image_compare.h"
#include "tests/render/png_io.h"
#include "world/gpu_backend.h"
#include "world/prefab_cache.h"
#include "world/world.h"

namespace blue::render_test {
namespace {

constexpr uint32_t kWidth = 960;
constexpr uint32_t kHeight = 720;
constexpr const char* kHdri = "env/kloofendal_48d_partly_cloudy_puresky_2k.hdr";

std::string Asset(const char* name) {
  return core::JoinPath(BLUE_ASSETS_DIR, name);
}

render::Camera LookAt(glm::vec3 eye, glm::vec3 target) {
  render::Camera camera;
  camera.position = eye;
  camera.view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
  camera.vertical_fov_radians = glm::radians(40.0f);
  camera.near_plane = 0.05f;
  camera.projection = camera::ReversedInfinitePerspective(
      camera.vertical_fov_radians,
      static_cast<float>(kWidth) / static_cast<float>(kHeight),
      camera.near_plane);
  camera.previous_view_projection = camera.projection * camera.view;
  return camera;
}

void Place(scene::SceneData& scene, const char* prefab, glm::vec3 position) {
  scene::Instance instance{.prefab = scene::AddPrefab(scene, Asset(prefab)),
                           .transform = {.position = position}};
  scene::AddInstance(scene, std::move(instance));
}

}  // namespace

TEST_CASE("Kyle figures preview" * doctest::skip()) {
  REQUIRE(!Options().backends.empty());
  render::Renderer renderer(render::RendererOptions{
      .window = nullptr,
      .backend = Options().backends.front(),
      .shader_dir = BLUE_SHADER_BLOB_DIR,
      .adapter = Options().adapter,
      .headless_extent = {kWidth, kHeight},
      .max_anisotropy = 4.0f,
  });
  auto sky = asset::LoadHdrFile(Asset(kHdri));
  REQUIRE(sky.ok());
  renderer.SetEnvironment(sky.value(), scene::kDefaultEnvironmentIntensity,
                          scene::kDefaultEnvironmentRotation);

  world::RendererBackend gpu(renderer);
  world::PrefabCache cache(gpu);
  world::World world(cache);
  scene::SceneData scene;
  Place(scene, "ground.pbo", {0.0f, 0.0f, -300.0f});
  Place(scene, "kyle_rifle.pbo", {-0.75f, 0.0f, 0.0f});
  Place(scene, "kyle_pistol.pbo", {0.75f, 0.0f, 0.0f});
  REQUIRE(world.Build(scene).empty());
  std::vector<render::DrawItem> draws;
  world.AppendDraws(draws);

  struct View {
    const char* name;
    glm::vec3 eye;
    glm::vec3 target;
  };
  // The figures face -Z: "front" looks back at them from down range.
  const View views[] = {
      {"front", {1.6f, 1.6f, -3.6f}, {0.0f, 1.0f, 0.0f}},
      {"left", {-3.6f, 1.5f, -1.2f}, {0.0f, 1.0f, -0.2f}},
  };
  for (const View& view : views) {
    render::SceneView scene_view;
    scene_view.camera = LookAt(view.eye, view.target);
    scene_view.draws = draws;
    scene_view.point_lights = world.point_lights();
    scene_view.sun.direction = scene::kDefaultSunDirection;
    scene_view.sun.color = scene::kDefaultSunColor;
    scene_view.sun.intensity = scene::kDefaultSunIntensity;
    scene_view.post.exposure_ev = scene::kDefaultExposureEv;
    scene_view.post.weapon_motion_blur_enabled = false;
    scene_view.frame_seconds = 1.0f / 60.0f;
    uint64_t ticket = 0;
    for (int frame = 0; frame < 3; ++frame) {
      if (frame == 2) {
        ticket = renderer.RequestOutputReadback();
      }
      renderer.DrawFrame(scene_view, nullptr);
    }
    renderer.WaitIdle();
    render::ViewportPixels pixels;
    REQUIRE(renderer.TakeViewportReadback(ticket, pixels) ==
            render::ReadbackStatus::kReady);
    RgbaImage image{.width = pixels.width, .height = pixels.height};
    image.rgba.resize(pixels.rgba.size());
    for (size_t i = 0; i < pixels.rgba.size(); ++i) {
      image.rgba[i] =
          (i % 4 == 3) ? uint8_t{255} : static_cast<uint8_t>(pixels.rgba[i]);
    }
    const std::string path =
        Options().out_dir + "/kyle_engine_" + view.name + ".png";
    CHECK(WritePng(path, image));
    MESSAGE("wrote ", path);
  }
}

}  // namespace blue::render_test

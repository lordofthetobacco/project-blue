// Offscreen preview of the ragdoll of a killed player (game::Ragdolls drawing
// kyle.pbo through game::CharacterModel), through the real renderer: one PNG
// per instant (0, 0.1, 0.25, 0.5, 1 and 2 s after the hit) for a hit from the
// left and from the front. Not a golden test: skipped unless asked for, it
// needs the cooked sample assets (blue_sample_assets) and writes PNGs for a
// person to look at:
//
//   blue_render_tests -tc="*ragdoll*" --no-skip --out=<dir>
//
// Files: <dir>/ragdoll_<sequence>_<milliseconds>ms.png

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "camera/projection.h"
#include "core/path.h"
#include "game/character_animator.h"
#include "game/character_model.h"
#include "game/ragdoll_rig.h"
#include "game/ragdolls.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "platform/file_system.h"
#include "render/backend.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "scene/default_lighting.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "tests/render/harness_options.h"
#include "tests/render/image_compare.h"
#include "tests/render/png_io.h"
#include "world/gpu_backend.h"
#include "world/prefab_cache.h"
#include "world/world.h"

namespace blue::render_test {
namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr float kDt = 1.0f / 60.0f;
constexpr const char* kHdri = "env/kloofendal_48d_partly_cloudy_puresky_2k.hdr";
constexpr std::array<int, 6> kMilliseconds = {0, 100, 250, 500, 1000, 2000};

std::string Asset(const char* name) {
  return core::JoinPath(BLUE_ASSETS_DIR, name);
}

render::Camera LookAt(glm::vec3 eye, glm::vec3 target, float fov_degrees) {
  render::Camera camera;
  camera.position = eye;
  camera.view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
  camera.vertical_fov_radians = glm::radians(fov_degrees);
  camera.near_plane = 0.02f;
  camera.projection = camera::ReversedInfinitePerspective(
      camera.vertical_fov_radians,
      static_cast<float>(kWidth) / static_cast<float>(kHeight),
      camera.near_plane);
  camera.previous_view_projection = camera.projection * camera.view;
  return camera;
}

// The renderer, a ground plane and the player model.
class Stage {
 public:
  Stage()
      : renderer_(render::RendererOptions{
            .window = nullptr,
            .backend = Options().backends.front(),
            .shader_dir = BLUE_SHADER_BLOB_DIR,
            .adapter = Options().adapter,
            .headless_extent = {kWidth, kHeight},
            .max_anisotropy = 4.0f,
        }),
        gpu_(renderer_),
        cache_(gpu_),
        world_(cache_) {
    auto sky = asset::LoadHdrFile(Asset(kHdri));
    REQUIRE(sky.ok());
    renderer_.SetEnvironment(sky.value(), scene::kDefaultEnvironmentIntensity,
                             scene::kDefaultEnvironmentRotation);
    scene::SceneData scene;
    scene::Instance ground{
        .prefab = scene::AddPrefab(scene, Asset("ground.pbo")),
        .transform = {.position = {0.0f, 0.0f, -300.0f}}};
    scene::AddInstance(scene, std::move(ground));
    REQUIRE(world_.Build(scene).empty());
    model_ = game::CharacterModel::Load(cache_, Asset("kyle.pbo"));
    REQUIRE(model_.has_value());
  }

  game::CharacterModel& model() { return *model_; }
  const prefab::Prefab& model_prefab() const {
    return cache_.prefab(model_->prefab());
  }

  // Renders the ground plus `extra` from `camera` into <out>/<name>.png.
  void Render(const std::string& name, const render::Camera& camera,
              const std::vector<render::DrawItem>& extra) {
    std::vector<render::DrawItem> draws;
    world_.AppendDraws(draws);
    draws.insert(draws.end(), extra.begin(), extra.end());
    render::SceneView view;
    view.camera = camera;
    view.draws = draws;
    view.point_lights = world_.point_lights();
    view.sun.direction = glm::normalize(scene::kDefaultSunDirection);
    view.sun.color = scene::kDefaultSunColor;
    view.sun.intensity = scene::kDefaultSunIntensity;
    view.post.exposure_ev = scene::kDefaultExposureEv;
    view.post.weapon_motion_blur_enabled = false;
    view.frame_seconds = kDt;
    uint64_t ticket = 0;
    for (int frame = 0; frame < 3; ++frame) {
      if (frame == 2) {
        ticket = renderer_.RequestOutputReadback();
      }
      renderer_.DrawFrame(view, nullptr);
    }
    renderer_.WaitIdle();
    render::ViewportPixels pixels;
    REQUIRE(renderer_.TakeViewportReadback(ticket, pixels) ==
            render::ReadbackStatus::kReady);
    RgbaImage image{.width = pixels.width, .height = pixels.height};
    image.rgba.resize(pixels.rgba.size());
    for (size_t i = 0; i < pixels.rgba.size(); ++i) {
      image.rgba[i] =
          (i % 4 == 3) ? uint8_t{255} : static_cast<uint8_t>(pixels.rgba[i]);
    }
    const std::string path = Options().out_dir + "/" + name + ".png";
    CHECK(WritePng(path, image));
    MESSAGE("wrote ", path);
  }

 private:
  render::Renderer renderer_;
  world::RendererBackend gpu_;
  world::PrefabCache cache_;
  world::World world_;
  std::optional<game::CharacterModel> model_;
};

// Shoots the standing rifleman along `direction` at the point `offset` from
// the chest and renders the fall. The figure stands at the origin facing -Z.
void Sequence(Stage& stage, const std::string& name, glm::vec3 offset,
              glm::vec3 direction) {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world(runtime, {.worker_threads = 0});
  world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  physics::BodyDesc ground;
  ground.body.shape = scene::ShapeType::kBox;
  ground.body.motion = scene::MotionType::kStatic;
  ground.bounds = asset::Bounds{glm::vec3(-50.0f, 0.0f, -50.0f),
                                glm::vec3(50.0f, 0.0f, 50.0f)};
  REQUIRE(world.CreateBody(ground).ok());

  game::Ragdolls ragdolls(world);
  const game::CharacterRig& rig = stage.model().rig();
  REQUIRE(ragdolls.SetModel(rig, stage.model_prefab()));

  game::CharacterAnimator animator(rig);
  game::CharacterInput input;
  input.dt = kDt;
  input.aiming = true;
  for (int frame = 0; frame < 60; ++frame) {
    animator.Update(input);
  }
  const auto ribs = animator.joint_position(game::Joint::kRibs);
  const glm::vec3 chest = glm::vec3(animator.root() * glm::vec4(ribs, 1.0f));
  const game::RagdollId id = ragdolls.Spawn(
      {.root = animator.root(),
       .locals = animator.locals(),
       .hit = game::RagdollHit{.point = chest + offset,
                               .velocity = direction * 800.0f,
                               .zone = game::RagdollZone::kTorso}});
  REQUIRE(id != 0);

  const render::Camera camera =
      LookAt(glm::vec3(2.6f, 1.3f, -3.2f), glm::vec3(0.0f, 0.65f, 0.0f), 40.0f);
  int step = 0;
  for (const int milliseconds : kMilliseconds) {
    const int target = static_cast<int>(
        std::lround(static_cast<float>(milliseconds) / 1000.0f / kDt));
    for (; step < target; ++step) {
      world.StepOnce();
      world.SetInterpolationAlpha(1.0f);
      ragdolls.Update(kDt);
    }
    const game::RagdollView* view = ragdolls.Find(id);
    REQUIRE(view != nullptr);
    std::vector<render::DrawItem> draws;
    stage.model().AppendDraws(glm::mat4(1.0f), view->locals, draws);
    stage.Render(std::format("ragdoll_{}_{}ms", name, milliseconds), camera,
                 draws);
  }

  const game::RagdollView* view = ragdolls.Find(id);
  REQUIRE(view != nullptr);
  CHECK(std::isfinite(view->center.x));
  CHECK(std::isfinite(view->center.y));
  CHECK(std::isfinite(view->center.z));
  // Lying on the ground: the pelvis within 40 cm of it.
  CHECK(view->center.y > -0.03f);
  CHECK(view->center.y < 0.4f);
}

}  // namespace

TEST_CASE("Ragdoll hit preview" * doctest::skip()) {
  REQUIRE(!Options().backends.empty());
  Stage stage;
  // The figure faces -Z, its left is -X: the bullet comes from the left
  // (flying along +X) or from the front (flying along +Z).
  Sequence(stage, "left", glm::vec3(-0.3f, 0.0f, 0.0f),
           glm::vec3(1.0f, 0.0f, 0.0f));
  Sequence(stage, "front", glm::vec3(0.0f, 0.0f, -0.3f),
           glm::vec3(0.0f, 0.0f, 1.0f));
}

}  // namespace blue::render_test

// Offscreen previews of the animated player model (game::CharacterAnimator
// driving kyle.pbo through game::CharacterModel), through the real renderer:
// gait strips, aiming, seams, the first-person player's own shadow and a
// remote player beside the static N15 figures. Not a golden test: skipped
// unless asked for, it needs the cooked sample assets (blue_sample_assets)
// and writes PNGs for a person to look at:
//
//   blue_render_tests -tc="*animated*" --no-skip --out=<dir>

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "asset/image_decoder.h"
#include "camera/projection.h"
#include "core/path.h"
#include "game/character_animator.h"
#include "game/character_model.h"
#include "platform/file_system.h"
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
#include "world/prefab_pose.h"
#include "world/world.h"

namespace blue::render_test {
namespace {

using game::CharacterAnimator;
using game::CharacterInput;
using game::HeldWeapon;
using game::WeaponHold;

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr float kDt = 1.0f / 60.0f;
constexpr const char* kHdri = "env/kloofendal_48d_partly_cloudy_puresky_2k.hdr";
// The first-person eye (player::PlayerTuning::eye_height_standing).
constexpr float kEyeHeight = 1.65f;
constexpr float kEyeHeightCrouched = 1.05f;

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

// One posed character to draw.
struct Pose {
  CharacterAnimator animator;
  std::optional<world::PrefabId> weapon;
};

// The renderer, the world (ground, optionally the N15 figures), the player
// model and the weapons.
class Stage {
 public:
  explicit Stage(bool figures)
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
    Place(scene, "ground.pbo", {0.0f, 0.0f, -300.0f});
    if (figures) {
      Place(scene, "kyle_rifle.pbo", {-0.9f, 0.0f, 0.0f});
      Place(scene, "kyle_pistol.pbo", {0.9f, 0.0f, 0.0f});
    }
    REQUIRE(world_.Build(scene).empty());
    model_ = game::CharacterModel::Load(cache_, Asset("kyle.pbo"));
    REQUIRE(model_.has_value());
    auto rifle = cache_.Get(Asset("m16.pbo"));
    auto pistol = cache_.Get(Asset("glock17.pbo"));
    REQUIRE(rifle.ok());
    REQUIRE(pistol.ok());
    rifle_ = rifle.value();
    pistol_ = pistol.value();
    rifle_hold_ =
        game::MakeWeaponHold(HeldWeapon::kRifle, cache_.prefab(rifle_));
    pistol_hold_ =
        game::MakeWeaponHold(HeldWeapon::kPistol, cache_.prefab(pistol_));
  }

  const game::CharacterRig& rig() const { return model_->rig(); }

  // The weapon prefab and hold of `kind`.
  std::optional<world::PrefabId> Prefab(HeldWeapon kind) const {
    if (kind == HeldWeapon::kNone) {
      return std::nullopt;
    }
    return kind == HeldWeapon::kRifle ? rifle_ : pistol_;
  }
  WeaponHold Hold(HeldWeapon kind) const {
    if (kind == HeldWeapon::kNone) {
      return {};
    }
    return kind == HeldWeapon::kRifle ? rifle_hold_ : pistol_hold_;
  }

  // Appends a first-person weapon of `kind` as the game shows it: in front
  // of the eye `eye` (looking along `look`), its grip down and right of the
  // view, pointing along the look; `casts_shadow` as App sets it.
  void AppendViewmodel(HeldWeapon kind, glm::vec3 eye, glm::vec3 look,
                       bool casts_shadow,
                       std::vector<render::DrawItem>& draws) {
    const WeaponHold hold = Hold(kind);
    const glm::vec3 forward = glm::normalize(look);
    const glm::vec3 right =
        glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, forward);
    // Prefab (forward, up, right) -> view (forward, up, right).
    const glm::vec3 hold_right = glm::cross(hold.forward, hold.up);
    const glm::mat3 from(hold.forward, hold.up, hold_right);
    const glm::mat3 to(forward, up, right);
    glm::mat4 root(to * glm::transpose(from));
    const glm::vec3 grip = eye + forward * 0.3f + right * 0.15f - up * 0.22f;
    root[3] = glm::vec4(grip - glm::mat3(root) * hold.grip, 1.0f);
    const size_t first = draws.size();
    const world::PrefabPose weapon(cache_, *Prefab(kind));
    weapon.AppendDraws(root, draws);
    for (size_t i = first; i < draws.size(); ++i) {
      draws[i].hidden_in_scope = true;
      draws[i].casts_shadow = casts_shadow;
    }
  }

  // Appends `pose`'s draws (body and weapon); `shadow_only` marks them so.
  void AppendCharacter(const Pose& pose, std::vector<render::DrawItem>& draws,
                       bool shadow_only = false) {
    const size_t first = draws.size();
    model_->AppendDraws(pose.animator, draws);
    if (pose.weapon && pose.animator.weapon()) {
      const world::PrefabPose weapon(cache_, *pose.weapon);
      weapon.AppendDraws(pose.animator.root() * *pose.animator.weapon(), draws);
    }
    for (size_t i = first; i < draws.size(); ++i) {
      draws[i].shadow_only = shadow_only;
    }
  }

  // Renders the world plus `extra` from `camera` into <out>/<name>.png.
  void Render(const std::string& name, const render::Camera& camera,
              const std::vector<render::DrawItem>& extra,
              glm::vec3 sun_direction = scene::kDefaultSunDirection) {
    std::vector<render::DrawItem> draws;
    world_.AppendDraws(draws);
    draws.insert(draws.end(), extra.begin(), extra.end());
    render::SceneView view;
    view.camera = camera;
    view.draws = draws;
    view.point_lights = world_.point_lights();
    view.sun.direction = glm::normalize(sun_direction);
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
  static void Place(scene::SceneData& scene, const char* prefab,
                    glm::vec3 position) {
    scene::Instance instance{.prefab = scene::AddPrefab(scene, Asset(prefab)),
                             .transform = {.position = position}};
    scene::AddInstance(scene, std::move(instance));
  }

  render::Renderer renderer_;
  world::RendererBackend gpu_;
  world::PrefabCache cache_;
  world::World world_;
  std::optional<game::CharacterModel> model_;
  world::PrefabId rifle_{};
  world::PrefabId pistol_{};
  WeaponHold rifle_hold_;
  WeaponHold pistol_hold_;
};

// What one character in a strip does.
struct Action {
  HeldWeapon weapon = HeldWeapon::kRifle;
  glm::vec3 velocity{0.0f};  // character space (yaw 0)
  float pitch = 0.0f;        // degrees
  bool aiming = false;
  bool crouching = false;
  bool on_ground = true;
  // >= 0: run until the gait phase crosses this; < 0: run `seconds`.
  float phase = -1.0f;
  float seconds = 1.0f;
};

Pose Animate(const Stage& stage, const Action& action, glm::vec3 feet) {
  Pose pose{.animator = CharacterAnimator(stage.rig()),
            .weapon = stage.Prefab(action.weapon)};
  CharacterInput input{.dt = kDt,
                       .feet = feet,
                       .velocity = action.velocity,
                       .pitch = glm::radians(action.pitch),
                       .crouching = action.crouching,
                       .on_ground = true,
                       .aiming = action.aiming,
                       .weapon = stage.Hold(action.weapon)};
  // Settle on the ground first (and walk two seconds into the gait).
  for (int frame = 0; frame < 120; ++frame) {
    pose.animator.Update(input);
  }
  input.on_ground = action.on_ground;
  if (action.phase >= 0.0f) {
    input.dt = 1.0f / 480.0f;
    float previous = pose.animator.gait_phase();
    for (int step = 0; step < 4800; ++step) {
      pose.animator.Update(input);
      const float now = pose.animator.gait_phase();
      const bool crossed = previous < now
                               ? previous < action.phase && action.phase <= now
                               : previous < action.phase || action.phase <= now;
      previous = now;
      if (crossed) {
        break;
      }
    }
  } else {
    const int frames = static_cast<int>(action.seconds / kDt);
    for (int frame = 0; frame < frames; ++frame) {
      pose.animator.Update(input);
    }
  }
  return pose;
}

// A short description of `action` for the strip's index.
std::string Describe(const Action& action) {
  const char* weapon = action.weapon == HeldWeapon::kRifle    ? "rifle"
                       : action.weapon == HeldWeapon::kPistol ? "pistol"
                                                              : "unarmed";
  std::string text = weapon;
  text += action.aiming
              ? " aimed"
              : (action.weapon == HeldWeapon::kNone ? "" : " low-ready");
  if (action.pitch != 0.0f) {
    text += std::format(" pitch {:+.0f}", action.pitch);
  }
  if (action.crouching) {
    text += " crouched";
  }
  if (!action.on_ground) {
    text += std::format(" airborne {:.2f} s", action.seconds);
  }
  const float speed = glm::length(action.velocity);
  if (speed > 0.0f) {
    const char* heading = action.velocity.z < -0.5f * speed  ? "forwards"
                          : action.velocity.z > 0.5f * speed ? "backwards"
                                                             : "sideways";
    text += std::format(" moving {} {:.1f} m/s", heading, speed);
  }
  if (action.phase >= 0.0f) {
    text += std::format(" gait phase {:.3f}", action.phase);
  }
  return text;
}

// Writes <out>/<name>.txt: what the image shows, left to right.
void WriteIndex(const std::string& name, const std::vector<std::string>& lines,
                const std::string& heading) {
  std::string text = heading + "\n";
  for (size_t i = 0; i < lines.size(); ++i) {
    text += std::format("{}. {}\n", i + 1, lines[i]);
  }
  const std::string path = Options().out_dir + "/" + name + ".txt";
  CHECK(platform::WriteFileAtomic(
            path, std::as_bytes(std::span(text.data(), text.size())))
            .ok());
}

// Renders `actions` side by side (`spacing` m apart, in a row across the
// view, left to right on screen), seen from `view_offset` relative to the
// middle of the row.
void Strip(Stage& stage, const std::string& name,
           const std::vector<Action>& actions, glm::vec3 view_offset,
           float spacing = 1.0f, float fov = 30.0f) {
  std::vector<render::DrawItem> draws;
  const float width = spacing * static_cast<float>(actions.size() - 1);
  // Screen right as seen from the camera, on the ground.
  const glm::vec3 row = glm::normalize(glm::cross(
      glm::vec3(-view_offset.x, 0.0f, -view_offset.z), glm::vec3(0, 1, 0)));
  for (size_t i = 0; i < actions.size(); ++i) {
    const glm::vec3 feet =
        row * (-0.5f * width + spacing * static_cast<float>(i));
    stage.AppendCharacter(Animate(stage, actions[i], feet), draws);
  }
  const glm::vec3 target(0.0f, 0.9f, 0.0f);
  stage.Render(name, LookAt(target + view_offset, target, fov), draws);
  std::vector<std::string> lines;
  for (const Action& action : actions) {
    lines.push_back(Describe(action));
  }
  WriteIndex(name, lines,
             "Figures left to right on screen (all face -Z, i.e. towards "
             "down range):");
}

std::vector<Action> Phases(Action base, int count) {
  std::vector<Action> actions;
  for (int i = 0; i < count; ++i) {
    Action action = base;
    action.phase = static_cast<float>(i) / static_cast<float>(count);
    actions.push_back(action);
  }
  return actions;
}

}  // namespace

TEST_CASE("Kyle animated player model preview" * doctest::skip()) {
  REQUIRE(!Options().backends.empty());
  Stage stage(/*figures=*/false);
  // Looking at the row's front from down range, a little from the right.
  const glm::vec3 front(1.5f, 0.6f, -7.5f);
  const glm::vec3 side(9.0f, 0.5f, -1.0f);
  const glm::vec3 three_quarter(5.5f, 1.2f, -6.5f);

  Strip(stage, "idle",
        {{.weapon = HeldWeapon::kNone},
         {.weapon = HeldWeapon::kRifle},
         {.weapon = HeldWeapon::kPistol},
         {.weapon = HeldWeapon::kRifle, .aiming = true},
         {.weapon = HeldWeapon::kPistol, .aiming = true}},
        three_quarter, 1.1f);
  const Action walk{.velocity = {0.0f, 0.0f, -2.0f}};
  Strip(stage, "walk_side", Phases(walk, 8), side * 1.4f, 0.8f);
  Strip(stage, "walk_front", Phases(walk, 4), front);
  const Action sprint{.velocity = {0.0f, 0.0f, -7.0f}};
  Strip(stage, "sprint_side", Phases(sprint, 8), side * 1.4f, 0.8f);
  const Action backwards{.velocity = {0.0f, 0.0f, 3.0f}};
  Strip(stage, "backwards_side", Phases(backwards, 4), side);
  const Action strafe{.velocity = {3.0f, 0.0f, 0.0f}};
  Strip(stage, "strafe_front", Phases(strafe, 4), front);
  Strip(stage, "crouch",
        {{.weapon = HeldWeapon::kRifle, .crouching = true},
         {.weapon = HeldWeapon::kRifle, .aiming = true, .crouching = true},
         {.weapon = HeldWeapon::kPistol, .aiming = true, .crouching = true},
         {.weapon = HeldWeapon::kNone, .crouching = true}},
        three_quarter, 1.1f);
  Action crouch_walk{.velocity = {0.0f, 0.0f, -1.8f}, .crouching = true};
  Strip(stage, "crouch_walk_side", Phases(crouch_walk, 4), side);
  Strip(stage, "airborne",
        {{.velocity = {0.0f, 0.0f, -4.0f}, .on_ground = false, .seconds = 0.3f},
         {.weapon = HeldWeapon::kPistol, .on_ground = false, .seconds = 0.3f},
         {.weapon = HeldWeapon::kNone, .on_ground = false, .seconds = 0.3f}},
        three_quarter);
  for (const auto& [weapon, label] :
       {std::pair{HeldWeapon::kRifle, "rifle"},
        std::pair{HeldWeapon::kPistol, "pistol"}}) {
    for (const bool aiming : {true, false}) {
      std::vector<Action> aims;
      for (const float pitch : {-60.0f, 0.0f, 60.0f}) {
        aims.push_back({.weapon = weapon, .pitch = pitch, .aiming = aiming});
      }
      const std::string name =
          std::string(label) + (aiming ? "_aim" : "_ready") + "_pitch";
      Strip(stage, name + "_side", aims, side * 0.6f, 1.1f);
      Strip(stage, name + "_front", aims, front * 0.55f, 1.1f);
    }
  }
  Action unarmed_walk{.weapon = HeldWeapon::kNone,
                      .velocity = {0.0f, 0.0f, -2.0f}};
  Strip(stage, "unarmed_walk_side", Phases(unarmed_walk, 4), side);
  Strip(stage, "unarmed_walk_front", Phases(unarmed_walk, 4), front);

  // Seams: the most bent joints up close.
  const auto close_up = [&](const std::string& name, const Action& action,
                            glm::vec3 target, glm::vec3 offset) {
    std::vector<render::DrawItem> draws;
    stage.AppendCharacter(Animate(stage, action, glm::vec3(0.0f)), draws);
    stage.Render(name, LookAt(target + offset, target, 30.0f), draws);
  };
  const Action deep{
      .weapon = HeldWeapon::kRifle, .aiming = true, .crouching = true};
  close_up("seam_hip_knee_side", deep, {0.0f, 0.45f, 0.0f},
           {1.4f, 0.2f, -0.4f});
  close_up("seam_hip_knee_back", deep, {0.0f, 0.45f, 0.0f},
           {-0.6f, 0.4f, 1.3f});
  close_up("seam_shoulder_elbow_rifle",
           {.weapon = HeldWeapon::kRifle, .aiming = true}, {0.0f, 1.3f, -0.1f},
           {-0.9f, 0.3f, -1.1f});
  close_up("seam_shoulder_elbow_pistol",
           {.weapon = HeldWeapon::kPistol, .pitch = 45.0f, .aiming = true},
           {0.0f, 1.35f, -0.2f}, {1.0f, 0.2f, -1.0f});
  close_up("seam_knee_sprint",
           {.velocity = {0.0f, 0.0f, -7.0f}, .phase = 0.75f},
           {0.0f, 0.6f, 0.0f}, {1.6f, 0.2f, 0.0f});

  // The cost of one animator update (walking, armed, aiming).
  CharacterAnimator animator(stage.rig());
  const CharacterInput input{.dt = kDt,
                             .velocity = {0.0f, 0.0f, -3.0f},
                             .pitch = 0.2f,
                             .aiming = true,
                             .weapon = stage.Hold(HeldWeapon::kRifle)};
  constexpr int kUpdates = 20000;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kUpdates; ++i) {
    animator.Update(input);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  std::vector<render::DrawItem> draws;
  stage.AppendCharacter({.animator = animator}, draws);
  MESSAGE("animator update: ", seconds / kUpdates * 1.0e6, " us; ",
          animator.locals().size(), " nodes; ", draws.size(),
          " draw items per body");
}

TEST_CASE("Kyle animated own shadow preview" * doctest::skip()) {
  REQUIRE(!Options().backends.empty());
  Stage stage(/*figures=*/false);
  // The sun low behind the player (who faces -Z): the shadow falls ahead.
  const glm::vec3 sun(0.15f, -0.5f, -0.85f);
  struct Case {
    const char* name;
    Action action;
    float eye;
  };
  const Case cases[] = {
      {"idle", {.weapon = HeldWeapon::kRifle}, kEyeHeight},
      {"stride",
       {.weapon = HeldWeapon::kRifle,
        .velocity = {0.0f, 0.0f, -3.0f},
        .phase = 0.3f},
       kEyeHeight},
      {"crouch",
       {.weapon = HeldWeapon::kPistol, .aiming = true, .crouching = true},
       kEyeHeightCrouched},
  };
  for (const Case& c : cases) {
    std::vector<render::DrawItem> body;
    stage.AppendCharacter(Animate(stage, c.action, glm::vec3(0.0f)), body,
                          /*shadow_only=*/true);
    const glm::vec3 eye(0.0f, c.eye, 0.0f);
    for (const auto& [label, down] :
         {std::pair{"forward", 0.0f}, std::pair{"down45", 45.0f}}) {
      const glm::vec3 look(0.0f, -std::sin(glm::radians(down)),
                           -std::cos(glm::radians(down)));
      // As the game draws it: the body only in the shadow maps, the
      // first-person weapon seen but casting no shadow.
      std::vector<render::DrawItem> draws = body;
      stage.AppendViewmodel(c.action.weapon, eye, look,
                            /*casts_shadow=*/false, draws);
      stage.Render(std::string("shadow_") + c.name + "_" + label,
                   LookAt(eye, eye + look, 60.0f), draws, sun);
      if (std::string_view(c.name) == "stride" && down > 0.0f) {
        // For comparison: the viewmodel casting too (the double shadow),
        // and the viewmodel alone, casting or not (the flag at work).
        std::vector<render::DrawItem> both = body;
        stage.AppendViewmodel(c.action.weapon, eye, look,
                              /*casts_shadow=*/true, both);
        stage.Render("shadow_stride_down45_viewmodel_casting",
                     LookAt(eye, eye + look, 60.0f), both, sun);
        for (const bool casts : {true, false}) {
          std::vector<render::DrawItem> alone;
          stage.AppendViewmodel(c.action.weapon, eye, look, casts, alone);
          stage.Render(casts ? "shadow_viewmodel_alone_casting"
                             : "shadow_viewmodel_alone_not_casting",
                       LookAt(eye, eye + look, 60.0f), alone, sun);
        }
      }
    }
    // The same from outside, body drawn too: what casts the shadow.
    std::vector<render::DrawItem> seen;
    stage.AppendCharacter(Animate(stage, c.action, glm::vec3(0.0f)), seen);
    stage.Render(std::string("shadow_") + c.name + "_outside",
                 LookAt({3.0f, 2.5f, -4.0f}, {0.0f, 0.6f, -1.5f}, 40.0f), seen,
                 sun);
  }
}

TEST_CASE("Kyle animated beside the N15 figures preview" * doctest::skip()) {
  REQUIRE(!Options().backends.empty());
  Stage stage(/*figures=*/true);
  std::vector<render::DrawItem> draws;
  stage.AppendCharacter(
      Animate(stage, {.weapon = HeldWeapon::kRifle}, {0.0f, 0.0f, 0.0f}),
      draws);
  stage.AppendCharacter(Animate(stage,
                                {.weapon = HeldWeapon::kPistol,
                                 .velocity = {0.0f, 0.0f, -2.0f},
                                 .phase = 0.25f},
                                {2.0f, 0.0f, 0.3f}),
                        draws);
  stage.Render("remote_beside_figures_front",
               LookAt({0.6f, 1.2f, -5.0f}, {0.6f, 0.9f, 0.0f}, 35.0f), draws);
  stage.Render("remote_beside_figures_side",
               LookAt({6.0f, 1.0f, 0.2f}, {0.6f, 0.9f, 0.0f}, 35.0f), draws);
  const std::vector<std::string> lines{
      "x = 2.0: animated, pistol low-ready, walking forwards 2 m/s",
      "x = 0.9: static N15 figure kyle_pistol",
      "x = 0.0: animated, rifle low-ready, idle",
      "x = -0.9: static N15 figure kyle_rifle"};
  WriteIndex("remote_beside_figures_front", lines,
             "Left to right on screen (all 1.8 m tall, facing -Z):");
}

}  // namespace blue::render_test

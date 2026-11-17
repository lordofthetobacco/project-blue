#include "player/player_controller.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>

#include "camera/look.h"
#include "input/action.h"
#include "input/input_frame.h"
#include "input/input_settings.h"
#include "physics/character.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

namespace blue::player {

namespace {

using input::Action;

constexpr float kStep = 1.0f / 60.0f;

physics::BodyDesc GroundDesc() {
  return {
      .body = {.motion = scene::MotionType::kStatic,
               .shape = scene::ShapeType::kBox},
      .pose = {},
      .scale = glm::vec3(1.0f),
      .bounds = {.min = {-50.0f, 0.0f, -50.0f}, .max = {50.0f, 0.0f, 50.0f}},
      .mesh = nullptr,
      .shape_key = "test:ground",
  };
}

// A world with a ground plane at y = 0 and a player spawned on it.
class Fixture {
 public:
  Fixture() : world_(runtime_) {
    world_.SetGravity({0.0f, -9.81f, 0.0f});
    REQUIRE(world_.CreateBody(GroundDesc()).ok());
    REQUIRE(player_.Spawn(world_, {}, glm::vec3(0.0f, 0.05f, 0.0f), {}).ok());
    Settle();
  }

  void Tick(const input::InputFrame& frame) {
    player_.Update(frame, settings_, kStep);
    world_.Update(kStep);
    player_.UpdateView(settings_, kStep);
  }
  void Tick(const input::InputFrame& frame, int count) {
    for (int i = 0; i < count; ++i) {
      Tick(frame);
    }
  }
  void Settle() { Tick({}, 30); }

  PlayerController& player() { return player_; }
  input::InputSettings& settings() { return settings_; }
  physics::PhysicsWorld& world() { return world_; }

 private:
  physics::PhysicsRuntime runtime_;
  physics::PhysicsWorld world_;
  input::InputSettings settings_;
  PlayerController player_;
};

input::InputFrame Held(std::initializer_list<Action> actions) {
  input::InputFrame frame;
  for (const Action action : actions) {
    frame.actions.SetHeld(action, true);
  }
  return frame;
}

// A frame where `actions` are held, without press edges.
input::InputFrame StillHeld(std::initializer_list<Action> actions) {
  input::InputFrame frame = Held(actions);
  frame.actions.ClearEdges();
  return frame;
}

}  // namespace

TEST_CASE("A spawned player settles standing on the ground") {
  Fixture f;
  CHECK(f.player().spawned());
  CHECK(f.player().on_ground());
  CHECK(f.player().feet_position().y == doctest::Approx(0.0f).epsilon(0.05));
  CHECK(f.player().view().eye_position.y ==
        doctest::Approx(f.player().tuning().eye_height_standing).epsilon(0.05));
}

TEST_CASE("Forward input at yaw 0 walks along -Z at walking speed") {
  Fixture f;
  const glm::vec3 start = f.player().feet_position();
  f.Tick(Held({Action::kMoveForward}), 60);
  const glm::vec3 moved = f.player().feet_position() - start;
  CHECK(moved.z < -3.0f);
  CHECK(moved.z > -4.5f);
  CHECK(std::abs(moved.x) < 0.05f);
  CHECK(f.player().horizontal_speed() ==
        doctest::Approx(f.player().tuning().walk_speed).epsilon(0.05));
}

TEST_CASE("Strafing follows the yaw") {
  Fixture f;
  f.player().Teleport(glm::vec3(0.0f, 0.05f, 0.0f),
                      {.yaw = glm::half_pi<float>()});  // facing -X
  f.Settle();
  const glm::vec3 start = f.player().feet_position();
  f.Tick(Held({Action::kMoveRight}), 60);
  const glm::vec3 moved = f.player().feet_position() - start;
  CHECK(moved.z < -3.0f);  // right of -X is -Z
  CHECK(std::abs(moved.x) < 0.05f);
}

TEST_CASE("Mouse look turns the view with sensitivity and invert") {
  Fixture f;
  f.settings().mouse_sensitivity = 0.01f;
  input::InputFrame frame;
  frame.look = true;
  frame.mouse_delta = {10.0f, -10.0f};
  f.Tick(frame);
  CHECK(f.player().look().yaw == doctest::Approx(-0.1f));
  CHECK(f.player().look().pitch == doctest::Approx(0.1f));
  CHECK(f.player().view().look == f.player().look());

  f.settings().invert_y = true;
  f.Tick(frame);
  CHECK(f.player().look().pitch == doctest::Approx(0.0f));

  frame.look = false;
  f.Tick(frame);
  CHECK(f.player().look().yaw == doctest::Approx(-0.2f));
}

TEST_CASE("Held sprint is faster than walking and ends on release") {
  Fixture f;
  f.Tick(Held({Action::kMoveForward, Action::kSprint}), 60);
  CHECK(f.player().sprinting());
  CHECK(f.player().horizontal_speed() ==
        doctest::Approx(f.player().tuning().sprint_speed).epsilon(0.05));

  f.Tick(StillHeld({Action::kMoveForward}), 30);
  CHECK_FALSE(f.player().sprinting());
  CHECK(f.player().horizontal_speed() ==
        doctest::Approx(f.player().tuning().walk_speed).epsilon(0.05));
}

TEST_CASE("Toggled sprint persists while moving and cancels when idle") {
  Fixture f;
  f.settings().sprint_toggle = true;
  f.Tick(Held({Action::kMoveForward, Action::kSprint}));  // press once
  f.Tick(StillHeld({Action::kMoveForward}), 60);
  CHECK(f.player().sprinting());

  f.Tick({}, 10);  // let go of everything
  f.Tick(StillHeld({Action::kMoveForward}), 30);
  CHECK_FALSE(f.player().sprinting());
}

TEST_CASE("Held crouch lowers the eye and slows the player, releasing stands") {
  Fixture f;
  const float standing_eye = f.player().view().eye_position.y;
  f.Tick(Held({Action::kCrouch, Action::kMoveForward}), 60);
  CHECK(f.player().crouching());
  CHECK(f.player().view().eye_position.y <
        f.player().tuning().eye_height_crouching + 0.1f);
  CHECK(f.player().view().eye_position.y < standing_eye - 0.4f);
  CHECK(f.player().horizontal_speed() ==
        doctest::Approx(f.player().tuning().crouch_speed).epsilon(0.05));

  f.Tick({}, 60);
  CHECK_FALSE(f.player().crouching());
  CHECK(f.player().view().eye_position.y ==
        doctest::Approx(standing_eye).epsilon(0.05));
}

TEST_CASE("Toggled crouch flips on each press") {
  Fixture f;
  f.settings().crouch_toggle = true;
  f.Tick(Held({Action::kCrouch}));
  f.Tick({}, 10);
  CHECK(f.player().crouching());
  f.Tick(Held({Action::kCrouch}));
  f.Tick({}, 10);
  CHECK_FALSE(f.player().crouching());
}

TEST_CASE("Jump leaves the ground and the player lands again") {
  Fixture f;
  f.Tick(Held({Action::kJump}));
  bool left_ground = false;
  float peak = 0.0f;
  for (int i = 0; i < 120; ++i) {
    f.Tick({});
    left_ground = left_ground || !f.player().on_ground();
    peak = std::max(peak, f.player().feet_position().y);
  }
  CHECK(left_ground);
  CHECK(peak > 0.8f);
  CHECK(f.player().on_ground());
  CHECK(f.player().feet_position().y == doctest::Approx(0.0f).epsilon(0.05));
}

TEST_CASE("Crouching suppresses jumping") {
  Fixture f;
  f.Tick(Held({Action::kCrouch}), 5);
  f.Tick(Held({Action::kCrouch, Action::kJump}));
  float peak = 0.0f;
  for (int i = 0; i < 60; ++i) {
    f.Tick(StillHeld({Action::kCrouch}));
    peak = std::max(peak, f.player().feet_position().y);
  }
  CHECK(peak < 0.1f);
}

TEST_CASE("Field of view widens while sprinting and returns afterwards") {
  Fixture f;
  f.settings().fov_degrees = 70.0f;
  f.settings().sprint_fov_boost_degrees = 10.0f;
  f.Tick({});
  CHECK(f.player().view().vertical_fov_degrees == doctest::Approx(70.0f));

  f.Tick(Held({Action::kMoveForward, Action::kSprint}), 120);
  CHECK(f.player().view().vertical_fov_degrees > 79.0f);
  f.Tick({}, 120);
  CHECK(f.player().view().vertical_fov_degrees < 71.0f);
}

TEST_CASE("Head bob is silent at zero intensity and visible otherwise") {
  Fixture f;
  f.settings().head_bob_intensity = 0.0f;
  f.Tick(Held({Action::kMoveForward}), 60);
  float min_y = 10.0f;
  float max_y = -10.0f;
  for (int i = 0; i < 60; ++i) {
    f.Tick(StillHeld({Action::kMoveForward}));
    const float eye = f.player().view().eye_position.y;
    min_y = std::min(min_y, eye);
    max_y = std::max(max_y, eye);
  }
  CHECK(max_y - min_y < 0.005f);
  CHECK(f.player().view().roll == 0.0f);

  f.settings().head_bob_intensity = 1.0f;
  f.Tick(StillHeld({Action::kMoveForward}), 60);
  min_y = 10.0f;
  max_y = -10.0f;
  float peak_x = 0.0f;
  for (int i = 0; i < 60; ++i) {
    f.Tick(StillHeld({Action::kMoveForward}));
    const float eye = f.player().view().eye_position.y;
    min_y = std::min(min_y, eye);
    max_y = std::max(max_y, eye);
    peak_x = std::max(peak_x, std::abs(f.player().view().eye_position.x));
  }
  CHECK(max_y - min_y > 0.02f);
  CHECK(peak_x > 0.005f);  // lateral sway, since the walk is along -Z
}

TEST_CASE("An empty frame keeps the player still") {
  Fixture f;
  const glm::vec3 start = f.player().feet_position();
  f.Tick({}, 60);
  CHECK(glm::length(f.player().feet_position() - start) < 0.01f);
  CHECK(f.player().horizontal_speed() < 0.01f);
}

TEST_CASE("The view matrix looks along the forward direction") {
  Fixture f;
  f.Tick({});
  const PlayerView& view = f.player().view();
  const glm::mat4 m = view.View();
  // The view-space -Z axis is the forward direction: row 2 of the rotation
  // holds -forward.
  const glm::vec3 forward = view.Forward();
  CHECK(m[0][2] == doctest::Approx(-forward.x).epsilon(1.0e-4));
  CHECK(m[1][2] == doctest::Approx(-forward.y).epsilon(1.0e-4));
  CHECK(m[2][2] == doctest::Approx(-forward.z).epsilon(1.0e-4));
  const scene::SceneCamera camera = view.ToSceneCamera(70.0f);
  CHECK(camera.position == view.eye_position);
  CHECK(camera.vertical_fov_degrees == 70.0f);
}

TEST_CASE("The player survives a world rebuild and a teleport") {
  Fixture f;
  f.world().RemoveAllBodies();
  REQUIRE(f.world().CreateBody(GroundDesc()).ok());
  f.player().OnWorldRebuilt();
  f.Tick({}, 30);
  CHECK(f.player().on_ground());

  f.player().Teleport(glm::vec3(5.0f, 1.0f, -2.0f), {.yaw = 1.0f});
  CHECK(f.player().look().yaw == 1.0f);
  CHECK(f.player().feet_position() == glm::vec3(5.0f, 1.0f, -2.0f));
  f.Tick({}, 60);
  CHECK(f.player().on_ground());
  CHECK(f.player().feet_position().x == doctest::Approx(5.0f).epsilon(0.01));
}

TEST_CASE("Aiming slows the player and blocks sprinting") {
  Fixture f;
  f.Tick(Held({Action::kMoveForward, Action::kSprint, Action::kAim}), 60);
  CHECK(f.player().aiming());
  CHECK_FALSE(f.player().sprinting());
  CHECK(f.player().horizontal_speed() ==
        doctest::Approx(f.player().tuning().aim_speed).epsilon(0.05));

  // Releasing the sights with sprint still held resumes the sprint.
  f.Tick(StillHeld({Action::kMoveForward, Action::kSprint}), 60);
  CHECK_FALSE(f.player().aiming());
  CHECK(f.player().sprinting());
}

TEST_CASE("Aiming cancels a toggled sprint") {
  Fixture f;
  f.settings().sprint_toggle = true;
  f.Tick(Held({Action::kMoveForward, Action::kSprint}));
  f.Tick(StillHeld({Action::kMoveForward}), 10);
  REQUIRE(f.player().sprinting());

  f.Tick(Held({Action::kMoveForward, Action::kAim}));
  CHECK_FALSE(f.player().sprinting());
  f.Tick(StillHeld({Action::kMoveForward}), 10);
  CHECK_FALSE(f.player().sprinting());
}

TEST_CASE("Aiming crouched keeps the slower crouch speed") {
  Fixture f;
  f.Tick(Held({Action::kMoveForward, Action::kCrouch, Action::kAim}), 60);
  CHECK(f.player().horizontal_speed() ==
        doctest::Approx(f.player().tuning().crouch_speed).epsilon(0.05));
}

TEST_CASE("Aiming narrows the field of view smoothly and returns") {
  Fixture f;
  f.settings().fov_degrees = 70.0f;
  f.Tick({});
  REQUIRE(f.player().view().vertical_fov_degrees == doctest::Approx(70.0f));

  f.Tick(Held({Action::kAim}));
  const float first = f.player().view().vertical_fov_degrees;
  CHECK(first < 70.0f);
  CHECK(first > 60.0f);                   // Blends in rather than snapping.
  f.Tick(StillHeld({Action::kAim}), 30);  // 0.5 s
  const float aimed = 70.0f * f.player().tuning().aim_fov_scale;
  CHECK(f.player().view().vertical_fov_degrees ==
        doctest::Approx(aimed).epsilon(0.01));
  CHECK(f.player().aim_fraction() == doctest::Approx(1.0f).epsilon(0.01));

  f.Tick({}, 30);
  CHECK(f.player().view().vertical_fov_degrees ==
        doctest::Approx(70.0f).epsilon(0.01));
  CHECK(f.player().aim_fraction() < 0.01f);
}

TEST_CASE("Aimed mouse look turns slower by the FOV tangent ratio") {
  Fixture f;
  f.settings().fov_degrees = 70.0f;
  f.settings().mouse_sensitivity = 0.001f;
  f.Tick(StillHeld({Action::kAim}), 60);  // Fully aimed.

  input::InputFrame frame = StillHeld({Action::kAim});
  frame.look = true;
  frame.mouse_delta = {-10.0f, 0.0f};  // Turn left.
  f.Tick(frame);
  const float aimed = 70.0f * f.player().tuning().aim_fov_scale;
  const float ratio = std::tan(glm::radians(aimed) * 0.5f) /
                      std::tan(glm::radians(70.0f) * 0.5f);
  CHECK(f.player().look().yaw == doctest::Approx(0.01f * ratio).epsilon(0.01));
  CHECK(f.player().look_delta().x ==
        doctest::Approx(0.01f * ratio).epsilon(0.01));
  CHECK(f.player().look_delta().y == 0.0f);
}

TEST_CASE("Look delta reports the mouse turn of the frame only") {
  Fixture f;
  f.settings().mouse_sensitivity = 0.01f;
  input::InputFrame frame;
  frame.look = true;
  frame.mouse_delta = {10.0f, -5.0f};  // Right and up.
  f.Tick(frame);
  CHECK(f.player().look_delta().x == doctest::Approx(-0.1f));
  CHECK(f.player().look_delta().y == doctest::Approx(0.05f));

  f.player().AddLookKick(0.02f, 0.01f);
  CHECK(f.player().look_delta().x == doctest::Approx(-0.1f));
  f.Tick({});
  CHECK(f.player().look_delta() == glm::vec2(0.0f));
}

TEST_CASE("A look kick turns the view up and left within the pitch clamp") {
  Fixture f;
  const camera::LookAngles before = f.player().look();
  f.player().AddLookKick(0.01f, 0.005f);
  CHECK(f.player().look().pitch == doctest::Approx(before.pitch + 0.01f));
  CHECK(f.player().look().yaw == doctest::Approx(before.yaw + 0.005f));
  f.Tick({});
  CHECK(f.player().view().look == f.player().look());

  f.player().AddLookKick(10.0f, 0.0f);
  CHECK(f.player().look().pitch == doctest::Approx(glm::radians(89.0f)));
  f.player().AddLookKick(-20.0f, 0.0f);
  CHECK(f.player().look().pitch == doctest::Approx(-glm::radians(89.0f)));
}

}  // namespace blue::player

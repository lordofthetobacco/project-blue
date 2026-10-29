#include "camera/camera_controller.h"

#include <doctest/doctest.h>

#include <cmath>

#include <glm/gtc/constants.hpp>

namespace blue::camera {

namespace {

constexpr float kEpsilon = 1.0e-5f;
constexpr float kMoveEpsilon = 1.0e-4f;

bool NearlyEqual(float a, float b, float epsilon = kEpsilon) {
  return std::abs(a - b) <= epsilon;
}

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b,
                 float epsilon = kEpsilon) {
  return glm::length(a - b) <= epsilon;
}

input::InputFrame LookFrame(glm::vec2 delta) {
  input::InputFrame frame;
  frame.mouse_delta = delta;
  frame.look = true;
  return frame;
}

input::InputFrame MoveFrame(glm::vec3 move) {
  input::InputFrame frame;
  frame.move = move;
  return frame;
}

// Camera at the origin looking down -Z, in the requested mode.
CameraController MakeCamera(CameraMode mode = CameraMode::kFly) {
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {0.0f, 0.0f, 0.0f}, .target = {0.0f, 0.0f, -1.0f}});
  camera.set_mode(mode);
  return camera;
}

}  // namespace

TEST_CASE("mouse right turns the view right, mouse up looks up") {
  const input::InputSettings settings;
  const float angle = 100.0f * settings.mouse_sensitivity;

  CameraController right = MakeCamera();
  right.Update(LookFrame({100.0f, 0.0f}), settings, 0.016f);
  CHECK(right.yaw() < 0.0f);
  CHECK(NearlyEqual(right.yaw(), -angle));
  CHECK(right.Forward().x > 0.0f);
  CHECK(right.Forward().z < 0.0f);
  CHECK(
      NearlyEqual(right.Forward(), {std::sin(angle), 0.0f, -std::cos(angle)}));

  CameraController up = MakeCamera();
  up.Update(LookFrame({0.0f, -100.0f}), settings,
            0.016f);  // SDL reports mouse-up as negative y.
  CHECK(up.pitch() > 0.0f);
  CHECK(up.Forward().y > 0.0f);
  CHECK(NearlyEqual(up.Forward(), {0.0f, std::sin(angle), -std::cos(angle)}));
}

TEST_CASE("invert Y flips only the vertical mapping") {
  const input::InputSettings normal;
  input::InputSettings inverted;
  inverted.invert_y = true;
  const float angle = 100.0f * normal.mouse_sensitivity;

  CameraController up = MakeCamera();
  up.Update(LookFrame({0.0f, -100.0f}), inverted, 0.016f);
  CHECK(up.pitch() < 0.0f);
  CHECK(NearlyEqual(up.Forward(), {0.0f, -std::sin(angle), -std::cos(angle)}));

  CameraController down = MakeCamera();
  down.Update(LookFrame({0.0f, 100.0f}), inverted, 0.016f);
  CHECK(down.pitch() > 0.0f);

  CameraController a = MakeCamera();
  CameraController b = MakeCamera();
  a.Update(LookFrame({250.0f, 0.0f}), normal, 0.016f);
  b.Update(LookFrame({250.0f, 0.0f}), inverted, 0.016f);
  CHECK(a.yaw() == b.yaw());
  CHECK(NearlyEqual(a.Forward(), b.Forward()));
}

TEST_CASE("mouse look is independent of how motion is split across frames") {
  const input::InputSettings settings;

  CameraController single = MakeCamera();
  single.Update(LookFrame({300.0f, -120.0f}), settings, 0.016f);

  CameraController split = MakeCamera();
  for (int i = 0; i < 100; ++i) {
    split.Update(LookFrame({3.0f, -1.2f}), settings,
                 0.001f * static_cast<float>(i + 1));
  }

  CHECK(NearlyEqual(single.yaw(), split.yaw()));
  CHECK(NearlyEqual(single.pitch(), split.pitch()));
}

TEST_CASE("mouse delta is not scaled by frame time") {
  const input::InputSettings settings;

  CameraController fast = MakeCamera();
  CameraController slow = MakeCamera();
  fast.Update(LookFrame({120.0f, -45.0f}), settings, 0.001f);
  slow.Update(LookFrame({120.0f, -45.0f}), settings, 0.5f);

  CHECK(fast.yaw() == slow.yaw());
  CHECK(fast.pitch() == slow.pitch());
}

TEST_CASE("pitch is clamped to 89 degrees and yaw wraps into (-pi, pi]") {
  const input::InputSettings settings;
  const float max_pitch = glm::radians(89.0f);

  CameraController camera = MakeCamera();
  camera.Update(LookFrame({0.0f, -1.0e6f}), settings, 0.016f);
  CHECK(NearlyEqual(camera.pitch(), max_pitch));
  CHECK(std::abs(camera.Forward().y) < 1.0f);

  camera.Update(LookFrame({0.0f, 1.0e6f}), settings, 0.016f);
  CHECK(NearlyEqual(camera.pitch(), -max_pitch));
  CHECK(std::abs(camera.Forward().y) < 1.0f);

  for (const float x : {1.0e5f, -1.0e5f, 2856.0f, -2856.0f, 12345.0f}) {
    camera.Update(LookFrame({x, 0.0f}), settings, 0.016f);
    CHECK(camera.yaw() > -glm::pi<float>());
    CHECK(camera.yaw() <= glm::pi<float>());
  }

  // A turn of exactly -pi (yaw += pi) must land on +pi, not -pi.
  input::InputSettings coarse;
  coarse.mouse_sensitivity = glm::pi<float>() / 4.0f;
  CameraController half = MakeCamera();
  half.Update(LookFrame({-4.0f, 0.0f}), coarse, 0.016f);
  CHECK(half.yaw() > 0.0f);
}

TEST_CASE("forward key moves along -Z at yaw 0 and follows pitch") {
  const input::InputSettings settings;

  CameraController camera = MakeCamera();
  camera.Update(MoveFrame({0.0f, 0.0f, 1.0f}), settings, 1.0f);
  CHECK(NearlyEqual(camera.position(), {0.0f, 0.0f, -camera.fly_speed()},
                    kMoveEpsilon));

  CameraController pitched = MakeCamera();
  pitched.Update(LookFrame({0.0f, -300.0f}), settings, 0.016f);
  const glm::vec3 before = pitched.position();
  pitched.Update(MoveFrame({0.0f, 0.0f, 1.0f}), settings, 1.0f);
  CHECK(pitched.position().y > before.y);
}

TEST_CASE("strafing moves along the view's right vector") {
  const input::InputSettings settings;
  CameraController camera = MakeCamera();
  camera.Update(MoveFrame({1.0f, 0.0f, 0.0f}), settings, 1.0f);
  CHECK(NearlyEqual(camera.position(), {camera.fly_speed(), 0.0f, 0.0f},
                    kMoveEpsilon));
}

TEST_CASE("diagonal movement is not faster than straight movement") {
  const input::InputSettings settings;

  CameraController straight = MakeCamera();
  CameraController diagonal = MakeCamera();
  straight.Update(MoveFrame({0.0f, 0.0f, 1.0f}), settings, 0.5f);
  diagonal.Update(MoveFrame({1.0f, 0.0f, 1.0f}), settings, 0.5f);

  CHECK(NearlyEqual(glm::length(diagonal.position()),
                    glm::length(straight.position()), kMoveEpsilon));
}

TEST_CASE("movement distance does not depend on frame rate") {
  const input::InputSettings settings;

  CameraController coarse = MakeCamera();
  coarse.Update(MoveFrame({0.3f, 0.0f, 1.0f}), settings, 1.0f);

  CameraController fine = MakeCamera();
  for (int i = 0; i < 100; ++i) {
    fine.Update(MoveFrame({0.3f, 0.0f, 1.0f}), settings, 0.01f);
  }

  CHECK(NearlyEqual(coarse.position(), fine.position(), kMoveEpsilon));
}

TEST_CASE("sprint multiplies speed by four") {
  const input::InputSettings settings;

  CameraController walk = MakeCamera();
  CameraController sprint = MakeCamera();
  input::InputFrame frame = MoveFrame({0.0f, 0.0f, 1.0f});
  walk.Update(frame, settings, 0.25f);
  frame.sprint = true;
  sprint.Update(frame, settings, 0.25f);

  CHECK(NearlyEqual(glm::length(sprint.position()),
                    4.0f * glm::length(walk.position()), kMoveEpsilon));
}

TEST_CASE("up and down move along world Y regardless of pitch") {
  const input::InputSettings settings;
  CameraController camera = MakeCamera();
  camera.Update(LookFrame({200.0f, -400.0f}), settings, 0.016f);
  const glm::vec3 before = camera.position();

  camera.Update(MoveFrame({0.0f, 1.0f, 0.0f}), settings, 1.0f);
  CHECK(NearlyEqual(camera.position() - before,
                    {0.0f, camera.fly_speed(), 0.0f}, kMoveEpsilon));

  camera.Update(MoveFrame({0.0f, -1.0f, 0.0f}), settings, 1.0f);
  CHECK(NearlyEqual(camera.position(), before, kMoveEpsilon));
}

TEST_CASE(
    "keyboard moves the fly camera whether or not the mouse is captured") {
  const input::InputSettings settings;

  CameraController released = MakeCamera();
  input::InputFrame frame = MoveFrame({0.0f, 0.0f, 1.0f});
  frame.mouse_delta = {500.0f, 500.0f};
  frame.look = false;
  released.Update(frame, settings, 1.0f);
  CHECK(NearlyEqual(released.position(), {0.0f, 0.0f, -released.fly_speed()},
                    kMoveEpsilon));
  CHECK(released.yaw() == 0.0f);  // look gates the mouse only
  CHECK(released.pitch() == 0.0f);

  CameraController captured = MakeCamera();
  frame.look = true;
  frame.mouse_delta = {0.0f, 0.0f};
  captured.Update(frame, settings, 1.0f);
  CHECK(NearlyEqual(captured.position(), released.position(), kMoveEpsilon));
}

TEST_CASE("orbit mode ignores keyboard movement") {
  const input::InputSettings settings;
  CameraController camera = MakeCamera(CameraMode::kOrbit);
  input::InputFrame frame = MoveFrame({1.0f, 1.0f, 1.0f});
  frame.sprint = true;
  camera.Update(frame, settings, 1.0f);
  CHECK(NearlyEqual(camera.position(), {0.0f, 0.0f, 0.0f}));
}

TEST_CASE("fly wheel scales speed by 1.2 per notch within clamps") {
  const input::InputSettings settings;
  CameraController camera = MakeCamera();
  const float start = camera.fly_speed();

  input::InputFrame frame;
  frame.wheel = 1.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(NearlyEqual(camera.fly_speed(), start * 1.2f));

  frame.wheel = -2.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(NearlyEqual(camera.fly_speed(), start * 1.2f / 1.44f));

  frame.wheel = 1000.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(camera.fly_speed() == 100.0f);

  frame.wheel = -1000.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(camera.fly_speed() == 0.25f);
}

TEST_CASE("orbit wheel scales distance by 0.9 per notch and keeps the target") {
  const input::InputSettings settings;
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}});
  camera.set_mode(CameraMode::kOrbit);

  input::InputFrame frame;
  frame.wheel = 1.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(NearlyEqual(camera.ToSceneCamera().target, {0.0f, 0.0f, 0.0f},
                    kMoveEpsilon));
  CHECK(NearlyEqual(glm::length(camera.position()), 9.0f, kMoveEpsilon));

  frame.wheel = -1000.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(NearlyEqual(glm::length(camera.position()), 500.0f, 1.0e-2f));
  CHECK(
      NearlyEqual(camera.ToSceneCamera().target, {0.0f, 0.0f, 0.0f}, 1.0e-3f));

  frame.wheel = 1000.0f;
  camera.Update(frame, settings, 0.016f);
  CHECK(NearlyEqual(glm::length(camera.position()), 0.25f, kMoveEpsilon));
}

TEST_CASE("orbit rotate keeps target fixed and distance constant") {
  const input::InputSettings settings;
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {3.0f, 2.0f, 4.0f}, .target = {1.0f, 0.5f, -1.0f}});
  camera.set_mode(CameraMode::kOrbit);
  const scene::SceneCamera before = camera.ToSceneCamera();
  const float distance = glm::length(before.target - before.position);

  for (const glm::vec2 delta :
       {glm::vec2{80.0f, -30.0f}, glm::vec2{-400.0f, 90.0f},
        glm::vec2{20.0f, 5000.0f}}) {
    camera.Update(LookFrame(delta), settings, 0.016f);
    const scene::SceneCamera after = camera.ToSceneCamera();
    CHECK(NearlyEqual(after.target, before.target, kMoveEpsilon));
    CHECK(NearlyEqual(glm::length(after.target - after.position), distance,
                      kMoveEpsilon));
  }
}

TEST_CASE("orbit drag right swings the view right around the target") {
  const input::InputSettings settings;
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}});
  camera.set_mode(CameraMode::kOrbit);

  camera.Update(LookFrame({100.0f, 0.0f}), settings, 0.016f);
  CHECK(camera.yaw() < 0.0f);
  CHECK(camera.Forward().x > 0.0f);  // view turned right...
  CHECK(camera.position().x <
        0.0f);  // ...so the camera swung to the left of the target
}

TEST_CASE("orbit pan moves target and position by the same vector") {
  const input::InputSettings settings;
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {0.0f, 0.0f, 10.0f}, .target = {0.0f, 0.0f, 0.0f}});
  camera.set_mode(CameraMode::kOrbit);
  const scene::SceneCamera before = camera.ToSceneCamera();

  input::InputFrame frame;
  frame.pan = true;
  frame.mouse_delta = {40.0f, 20.0f};
  camera.Update(frame, settings, 0.016f);
  const scene::SceneCamera after = camera.ToSceneCamera();

  const glm::vec3 position_shift = after.position - before.position;
  const glm::vec3 target_shift = after.target - before.target;
  CHECK(NearlyEqual(position_shift, target_shift, kMoveEpsilon));
  CHECK(glm::length(position_shift) > 0.0f);
  CHECK(position_shift.x <
        0.0f);  // drag right: scene follows the cursor, camera goes left
  CHECK(position_shift.y > 0.0f);  // drag down: camera goes up
  CHECK(NearlyEqual(camera.yaw(), 0.0f));
  CHECK(NearlyEqual(camera.pitch(), 0.0f));
}

TEST_CASE("orbit pan speed scales with distance") {
  const input::InputSettings settings;
  input::InputFrame frame;
  frame.pan = true;
  frame.mouse_delta = {50.0f, 0.0f};

  CameraController near_camera;
  near_camera.SetFromSceneCamera(
      {.position = {0.0f, 0.0f, 2.0f}, .target = {0.0f, 0.0f, 0.0f}});
  near_camera.set_mode(CameraMode::kOrbit);
  near_camera.Update(frame, settings, 0.016f);

  CameraController far_camera;
  far_camera.SetFromSceneCamera(
      {.position = {0.0f, 0.0f, 20.0f}, .target = {0.0f, 0.0f, 0.0f}});
  far_camera.set_mode(CameraMode::kOrbit);
  far_camera.Update(frame, settings, 0.016f);

  CHECK(NearlyEqual(std::abs(far_camera.position().x),
                    10.0f * std::abs(near_camera.position().x), kMoveEpsilon));
}

TEST_CASE(
    "scene camera round trip preserves position, direction and distance") {
  const scene::SceneCamera input{.position = {2.2f, 1.6f, 2.8f},
                                 .target = {0.3f, -0.4f, 0.1f},
                                 .vertical_fov_degrees = 47.0f};

  CameraController camera;
  camera.SetFromSceneCamera(input);
  const scene::SceneCamera output = camera.ToSceneCamera();

  CHECK(NearlyEqual(output.position, input.position, kMoveEpsilon));
  CHECK(NearlyEqual(output.target, input.target, kMoveEpsilon));
  CHECK(output.vertical_fov_degrees == 47.0f);
  CHECK(camera.vertical_fov_degrees() == 47.0f);
}

TEST_CASE("degenerate scene camera falls back to looking down -Z") {
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {1.0f, 2.0f, 3.0f}, .target = {1.0f, 2.0f, 3.0f}});

  CHECK(NearlyEqual(camera.Forward(), {0.0f, 0.0f, -1.0f}));
  CHECK(NearlyEqual(camera.ToSceneCamera().target, {1.0f, 2.0f, 2.0f}));
}

TEST_CASE("switching modes keeps the view identical") {
  const input::InputSettings settings;
  CameraController camera;
  camera.SetFromSceneCamera(
      {.position = {2.0f, 1.0f, 3.0f}, .target = {0.0f, 0.5f, 0.0f}});
  camera.Update(LookFrame({60.0f, -25.0f}), settings, 0.016f);

  const glm::mat4 fly_view = camera.View();
  const glm::vec3 fly_position = camera.position();
  camera.set_mode(CameraMode::kOrbit);
  CHECK(camera.mode() == CameraMode::kOrbit);

  const glm::mat4 orbit_view = camera.View();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      CHECK(NearlyEqual(fly_view[column][row], orbit_view[column][row]));
    }
  }

  // An idle orbit update must not drift the camera either.
  camera.Update(input::InputFrame{}, settings, 0.016f);
  CHECK(NearlyEqual(camera.position(), fly_position, kMoveEpsilon));
  camera.set_mode(CameraMode::kFly);
  CHECK(NearlyEqual(camera.position(), fly_position, kMoveEpsilon));
}

}  // namespace blue::camera

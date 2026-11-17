#include "weapon/viewmodel.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "camera/look.h"
#include "weapon/rifle_geometry.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;
constexpr float kG = 9.81f;

ViewmodelInput Still(bool aim, camera::LookAngles look = {},
                     glm::vec3 eye = glm::vec3(0.0f), float roll = 0.0f) {
  ViewmodelInput input;
  input.eye_position = eye;
  input.look = look;
  input.roll = roll;
  input.aim_held = aim;
  input.dt = kFrame;
  return input;
}

// Runs `seconds` of identical frames.
void Settle(Viewmodel& viewmodel, const ViewmodelInput& input,
            float seconds = 2.0f) {
  const int frames = static_cast<int>(seconds / input.dt + 0.5f);
  for (int i = 0; i < frames; ++i) {
    viewmodel.Update(input);
  }
}

glm::vec3 ToWorld(const Viewmodel& viewmodel, glm::vec3 model_point) {
  return glm::vec3(viewmodel.model_matrix() * glm::vec4(model_point, 1.0f));
}

// Returns the distance of `point` from the ray `origin` + t * `direction`
// and its signed distance `along` it.
float DistanceFromAxis(glm::vec3 origin, glm::vec3 direction, glm::vec3 point,
                       float& along) {
  along = glm::dot(point - origin, direction);
  return glm::length(point - origin - along * direction);
}

// Right / up / forward of an eye with `look` and `roll`, as PlayerView.
struct EyeFrame {
  glm::vec3 right;
  glm::vec3 up;
  glm::vec3 forward;
};

EyeFrame FrameOf(camera::LookAngles look, float roll) {
  const glm::vec3 forward = camera::ForwardFrom(look);
  const glm::vec3 right = glm::normalize(glm::cross(forward, camera::kWorldUp));
  const glm::vec3 up = glm::cross(right, forward);
  const glm::vec3 rolled_up = std::cos(roll) * up + std::sin(roll) * right;
  return {glm::cross(forward, rolled_up), rolled_up, forward};
}

TEST_CASE("aimed, the optic axis runs through the eye along the view") {
  const RifleGeometry& rifle = M16Geometry();
  for (const camera::LookAngles look :
       {camera::LookAngles{}, camera::LookAngles{2.3f, 0.4f},
        camera::LookAngles{-1.1f, -0.9f}}) {
    for (const float roll : {0.0f, 0.03f}) {
      CAPTURE(look.yaw);
      CAPTURE(roll);
      const glm::vec3 eye(3.0f, 1.65f, -7.0f);
      Viewmodel viewmodel(rifle);
      Settle(viewmodel, Still(true, look, eye, roll));
      REQUIRE(viewmodel.aim_fraction() == 1.0f);
      const glm::vec3 forward = camera::ForwardFrom(look);
      float eye_side_along = 0.0f;
      float ahead_along = 0.0f;
      CHECK(DistanceFromAxis(eye, forward,
                             ToWorld(viewmodel, rifle.optic_eye_side),
                             eye_side_along) < 1.0e-4f);
      // A point half a metre down the optic axis, past the front sight.
      const glm::vec3 ahead =
          rifle.optic_eye_side + rifle.optic_axis * (0.5f / rifle.scale);
      CHECK(DistanceFromAxis(eye, forward, ToWorld(viewmodel, ahead),
                             ahead_along) < 1.0e-4f);
      CHECK(eye_side_along == doctest::Approx(viewmodel.tuning().eye_relief));
      CHECK(ahead_along == doctest::Approx(eye_side_along + 0.5f));
      CHECK(glm::dot(viewmodel.optic_axis(), forward) > 1.0f - 1.0e-6f);
      // Upright with the eye's roll: the muzzle hangs below the axis.
      const EyeFrame frame = FrameOf(look, roll);
      const glm::vec3 muzzle = viewmodel.muzzle_position() - eye;
      CHECK(glm::dot(muzzle, frame.up) < -0.05f);
      CHECK(std::abs(glm::dot(muzzle, frame.right)) < 1.0e-4f);
    }
  }
}

TEST_CASE("the lens is posed on the rifle and the dot shows only through it") {
  const RifleGeometry& rifle = M16Geometry();
  const camera::LookAngles look{0.6f, 0.2f};
  const float roll = 0.02f;
  const glm::vec3 eye(1.0f, 1.7f, 2.0f);
  Viewmodel viewmodel(rifle);
  Settle(viewmodel, Still(true, look, eye, roll));
  const EyeFrame frame = FrameOf(look, roll);
  const float radius = viewmodel.lens_radius();
  CHECK(radius == doctest::Approx(rifle.optic_lens_radius * rifle.scale));
  CHECK(glm::dot(viewmodel.lens_right(), frame.right) > 1.0f - 1.0e-5f);
  CHECK(glm::dot(viewmodel.lens_up(), frame.up) > 1.0f - 1.0e-5f);
  CHECK(glm::dot(viewmodel.optic_axis(), frame.forward) > 1.0f - 1.0e-6f);
  CHECK(glm::dot(viewmodel.lens_center() - eye, frame.forward) ==
        doctest::Approx(viewmodel.tuning().eye_relief +
                        (rifle.optic_lens.z - rifle.optic_eye_side.z) *
                            rifle.scale));

  const glm::mat4 lens = viewmodel.lens_matrix();
  const glm::vec3 corner(lens * glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
  CHECK(glm::length(corner - viewmodel.lens_center() -
                    radius * (viewmodel.lens_right() + viewmodel.lens_up())) <
        1.0e-6f);

  // The dot is where the ray from an eye along the optic axis meets the lens
  // plane. Collimated: moving the eye moves that point by the same offset,
  // so the dot stays on the same distant point until it leaves the disc.
  auto dot_on_lens = [&](glm::vec3 from) {
    const glm::vec3 axis = viewmodel.optic_axis();
    const float t = glm::dot(viewmodel.lens_center() - from, axis);
    const glm::vec3 point = from + t * axis - viewmodel.lens_center();
    return glm::vec2(glm::dot(point, viewmodel.lens_right()),
                     glm::dot(point, viewmodel.lens_up()));
  };
  const glm::vec2 centred = dot_on_lens(eye);
  CHECK(std::abs(centred.x) < 1.0e-5f);
  // Seen through the A2 notch: above the lens centre, inside the disc.
  CHECK(centred.y ==
        doctest::Approx((rifle.optic_eye_side.y - rifle.optic_lens.y) *
                        rifle.scale)
            .epsilon(1.0e-3));
  CHECK(glm::length(centred) < radius);
  const glm::vec2 shifted = dot_on_lens(eye - 0.005f * frame.right);
  CHECK(shifted.x == doctest::Approx(-0.005f).epsilon(1.0e-3));
  CHECK(glm::length(shifted) < radius);
  CHECK(glm::length(dot_on_lens(eye - 0.015f * frame.right)) > radius);
}

TEST_CASE("the zeroed bore crosses the line of sight at the zero distance") {
  const RifleGeometry& rifle = M16Geometry();
  for (const float zero : {50.0f, 100.0f, 300.0f}) {
    CAPTURE(zero);
    ViewmodelTuning tuning;
    tuning.zero_distance = zero;
    Viewmodel viewmodel(rifle, tuning);
    Settle(viewmodel, Still(true));
    const glm::vec3 muzzle = viewmodel.muzzle_position();
    const glm::vec3 direction = viewmodel.muzzle_direction();
    CHECK(glm::length(direction) == doctest::Approx(1.0f));
    // Pitched up from the sight line (-Z) by the zero angle.
    CHECK(direction.y > 0.0f);
    CHECK(std::acos(-direction.z) ==
          doctest::Approx(viewmodel.zero_angle()).epsilon(1.0e-3));
    CHECK(viewmodel.zero_angle() > 0.0f);
    CHECK(viewmodel.zero_angle() < 0.01f);
    // Drag-free flight: height above the line of sight (y = 0) when the
    // bullet is `distance` metres ahead of the eye.
    const glm::vec3 velocity = tuning.muzzle_velocity * direction;
    auto height_at = [&](float distance) {
      const float time = (-distance - muzzle.z) / velocity.z;
      return muzzle.y + velocity.y * time - 0.5f * kG * time * time;
    };
    CHECK(muzzle.y < 0.0f);
    CHECK(std::abs(height_at(zero)) < 1.0e-3f);
    // It really crosses there (rising through a near zero, falling through a
    // far one), rather than touching the line.
    CHECK(height_at(0.9f * zero) * height_at(1.1f * zero) < 0.0f);
  }
}

TEST_CASE("at the hip the muzzle is right of and below the view axis") {
  Viewmodel viewmodel(M16Geometry());
  Settle(viewmodel, Still(false));
  CHECK(viewmodel.aim_fraction() == 0.0f);
  const glm::vec3 muzzle = viewmodel.muzzle_position();
  CHECK(muzzle.x > 0.03f);
  CHECK(muzzle.y < -0.1f);
  CHECK(muzzle.z < -0.5f);
  // The bore roughly follows the view.
  CHECK(-viewmodel.muzzle_direction().z > std::cos(0.1f));
  CHECK(viewmodel.grip_position().y < muzzle.y);
}

TEST_CASE("the aim blend is monotonic, quick and frame-rate independent") {
  Viewmodel coarse(M16Geometry());
  Viewmodel fine(M16Geometry());
  ViewmodelInput fine_input = Still(true);
  fine_input.dt = kFrame / 4.0f;
  float previous = 0.0f;
  for (int frame = 0; frame < 12; ++frame) {
    coarse.Update(Still(true));
    for (int i = 0; i < 4; ++i) {
      fine.Update(fine_input);
    }
    CHECK(coarse.aim_fraction() >= previous);
    CHECK(fine.aim_fraction() ==
          doctest::Approx(coarse.aim_fraction()).epsilon(1.0e-4));
    previous = coarse.aim_fraction();
  }
  // 0.2 s: ~95 % of the way.
  CHECK(previous > 0.9f);
  CHECK(previous < 1.0f);
  Settle(coarse, Still(false));
  CHECK(coarse.aim_fraction() == 0.0f);
}

TEST_CASE("sprinting and reloading prevent aiming") {
  Viewmodel viewmodel(M16Geometry());
  ViewmodelInput input = Still(true);
  input.sprinting = true;
  Settle(viewmodel, input);
  CHECK(viewmodel.aim_fraction() == 0.0f);
  input.sprinting = false;
  input.reloading = true;
  Settle(viewmodel, input);
  CHECK(viewmodel.aim_fraction() == 0.0f);
}

TEST_CASE("sprint lowers the rifle and points it away") {
  Viewmodel hip(M16Geometry());
  Viewmodel sprint(M16Geometry());
  Settle(hip, Still(false));
  ViewmodelInput input = Still(false);
  input.sprinting = true;
  Settle(sprint, input);
  CHECK(sprint.muzzle_position().y < hip.muzzle_position().y - 0.1f);
  CHECK(-sprint.muzzle_direction().z < std::cos(0.3f));
}

TEST_CASE("recoil kicks the rifle back and up, then recovers") {
  Viewmodel viewmodel(M16Geometry());
  const ViewmodelInput still = Still(true);
  Settle(viewmodel, still);
  const glm::vec3 rest_muzzle = viewmodel.muzzle_position();
  const glm::vec3 rest_direction = viewmodel.muzzle_direction();
  ViewmodelInput shot = still;
  shot.shots = 1;
  viewmodel.Update(shot);
  viewmodel.Update(still);
  CHECK(viewmodel.muzzle_position().z > rest_muzzle.z + 0.005f);      // back
  CHECK(viewmodel.muzzle_direction().y > rest_direction.y + 0.005f);  // up
  Settle(viewmodel, still, 1.0f);
  CHECK(glm::length(viewmodel.muzzle_position() - rest_muzzle) < 1.0e-4f);
  CHECK(glm::length(viewmodel.muzzle_direction() - rest_direction) < 1.0e-4f);
}

TEST_CASE("TakeLookKick is deterministic per seed and clears on read") {
  auto kicks_for = [](uint32_t seed) {
    ViewmodelTuning tuning;
    tuning.seed = seed;
    Viewmodel viewmodel(M16Geometry(), tuning);
    ViewmodelInput shot = Still(false);
    shot.shots = 1;
    glm::vec2 kicks[5];
    for (glm::vec2& kick : kicks) {
      viewmodel.Update(shot);
      kick = viewmodel.TakeLookKick();
    }
    CHECK(viewmodel.TakeLookKick() == glm::vec2(0.0f));
    return std::to_array(kicks);
  };
  const auto first = kicks_for(7);
  const auto again = kicks_for(7);
  const auto other = kicks_for(8);
  bool spread_differs = false;
  bool yaw_varies = false;
  for (size_t i = 0; i < first.size(); ++i) {
    CHECK(first[i] == again[i]);
    CHECK(first[i].x == doctest::Approx(ViewmodelTuning{}.look_kick_pitch));
    CHECK(std::abs(first[i].y) <= ViewmodelTuning{}.look_kick_yaw);
    spread_differs = spread_differs || first[i].y != other[i].y;
    yaw_varies = yaw_varies || first[i].y != first[0].y;
  }
  CHECK(spread_differs);
  CHECK(yaw_varies);
}

TEST_CASE("aiming reduces the recoil kick") {
  ViewmodelInput shot = Still(true);
  shot.shots = 1;
  Viewmodel aimed(M16Geometry());
  Settle(aimed, Still(true));
  aimed.Update(shot);
  Viewmodel hip(M16Geometry());
  shot.aim_held = false;
  hip.Update(shot);
  CHECK(aimed.TakeLookKick().x < hip.TakeLookKick().x);
}

TEST_CASE("the model matrix is finite and carries the model scale") {
  const RifleGeometry& rifle = M16Geometry();
  Viewmodel viewmodel(rifle);
  ViewmodelInput input = Still(false, {0.7f, -0.3f}, glm::vec3(5.0f), 0.02f);
  input.velocity = glm::vec3(3.0f, 0.0f, 1.0f);
  input.look_delta = glm::vec2(0.02f, -0.01f);
  input.shots = 2;
  Settle(viewmodel, input, 0.5f);
  const glm::mat4 model = viewmodel.model_matrix();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      CHECK(std::isfinite(model[column][row]));
    }
  }
  for (int axis = 0; axis < 3; ++axis) {
    CHECK(glm::length(glm::vec3(model[axis])) ==
          doctest::Approx(rifle.scale).epsilon(1.0e-4));
  }
}

TEST_CASE("the rifle is rigidly attached to the eye frame") {
  // The same camera-space pose for any eye placement, yaw, pitch and roll.
  const RifleGeometry& rifle = M16Geometry();
  Viewmodel reference(rifle);
  Settle(reference, Still(false));
  const glm::vec3 local_muzzle = reference.muzzle_position();
  const glm::vec3 local_direction = reference.muzzle_direction();
  const camera::LookAngles look{2.0f, 0.5f};
  const float roll = 0.05f;
  const glm::vec3 eye(-4.0f, 2.0f, 9.0f);
  Viewmodel turned(rifle);
  Settle(turned, Still(false, look, eye, roll));
  const EyeFrame frame = FrameOf(look, roll);
  const glm::vec3 muzzle = turned.muzzle_position() - eye;
  CHECK(glm::dot(muzzle, frame.right) ==
        doctest::Approx(local_muzzle.x).epsilon(1.0e-4));
  CHECK(glm::dot(muzzle, frame.up) ==
        doctest::Approx(local_muzzle.y).epsilon(1.0e-4));
  CHECK(glm::dot(muzzle, frame.forward) ==
        doctest::Approx(-local_muzzle.z).epsilon(1.0e-4));
  const glm::vec3 direction = turned.muzzle_direction();
  CHECK(glm::dot(direction, frame.forward) ==
        doctest::Approx(-local_direction.z).epsilon(1.0e-5));
  CHECK(glm::dot(direction, frame.right) ==
        doctest::Approx(local_direction.x).epsilon(1.0e-3));
}

TEST_CASE("sway lags behind turning and settles; aiming reduces it") {
  Viewmodel hip(M16Geometry());
  Viewmodel aimed(M16Geometry());
  Settle(hip, Still(false));
  Settle(aimed, Still(true));
  const glm::vec3 hip_rest = hip.muzzle_direction();
  const glm::vec3 aimed_rest = aimed.muzzle_direction();
  ViewmodelInput turn = Still(false);
  turn.look_delta = glm::vec2(0.03f, 0.0f);  // turning left
  hip.Update(turn);
  turn.aim_held = true;
  aimed.Update(turn);
  // Lagging behind a left turn: the muzzle points right of its rest.
  CHECK(hip.muzzle_direction().x > hip_rest.x + 1.0e-3f);
  CHECK(aimed.muzzle_direction().x - aimed_rest.x <
        hip.muzzle_direction().x - hip_rest.x);
  Settle(hip, Still(false), 1.0f);
  CHECK(glm::length(hip.muzzle_direction() - hip_rest) < 1.0e-4f);
}

TEST_CASE("walking bobs the rifle; standing still does not") {
  Viewmodel viewmodel(M16Geometry());
  Settle(viewmodel, Still(false));
  const glm::vec3 rest = viewmodel.muzzle_position();
  ViewmodelInput walk = Still(false);
  walk.velocity = glm::vec3(0.0f, 0.0f, -4.0f);
  float largest = 0.0f;
  for (int i = 0; i < 120; ++i) {
    viewmodel.Update(walk);
    largest =
        std::max(largest, glm::length(viewmodel.muzzle_position() - rest));
  }
  CHECK(largest > 0.004f);
  CHECK(largest < 0.05f);
  Settle(viewmodel, Still(false));
  CHECK(glm::length(viewmodel.muzzle_position() - rest) < 1.0e-3f);
}

}  // namespace
}  // namespace blue::weapon

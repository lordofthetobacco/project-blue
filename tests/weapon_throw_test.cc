#include "weapon/weapon_throw.h"

#include <doctest/doctest.h>

#include <optional>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/mesh_data.h"

namespace blue::weapon {
namespace {

const glm::vec3 kForward(0.0f, 0.0f, -1.0f);
const glm::vec3 kUp(0.0f, 1.0f, 0.0f);

bool Near(glm::vec3 a, glm::vec3 b, float eps = 1.0e-4f) {
  return glm::length(a - b) <= eps;
}

// A rifle-like box (prefab space: 1 m long along -z, centred 0.3 m ahead of
// its origin) held 0.2 m right, 0.15 m below and 0.25 m ahead of an eye at
// 1.6 m, turned a little about y.
ThrowInput Rifle() {
  ThrowInput input;
  input.held = glm::rotate(
      glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, 1.45f, -0.25f)), 0.1f,
      kUp);
  input.bounds = {.min = {-0.03f, -0.1f, -0.8f}, .max = {0.03f, 0.1f, 0.2f}};
  input.eye = glm::vec3(0.0f, 1.6f, 0.0f);
  input.forward = kForward;
  input.player_velocity = glm::vec3(1.0f, 0.0f, -2.0f);
  return input;
}

TEST_CASE("a free throw leaves from the hands with the throw velocity") {
  const ThrowInput input = Rifle();
  const ThrowTuning tuning;
  const ThrowPlan plan = PlanThrow(input, tuning, std::nullopt);
  CHECK_FALSE(plan.blocked);
  CHECK(Near(plan.position, glm::vec3(input.held[3])));
  CHECK(Near(plan.scale, glm::vec3(1.0f)));
  const glm::quat expected = glm::angleAxis(0.1f, kUp);
  CHECK(std::abs(glm::dot(plan.rotation, expected)) ==
        doctest::Approx(1.0f).epsilon(1.0e-5));
  CHECK(Near(
      plan.linear_velocity,
      input.player_velocity + kForward * tuning.speed + kUp * tuning.lift));
  CHECK(Near(plan.angular_velocity, kUp * tuning.spin));
  // The body starts exactly where the model was drawn.
  const glm::mat4 matrix = PlanMatrix(plan);
  for (int i = 0; i < 4; ++i) {
    CHECK(Near(glm::vec3(matrix[i]), glm::vec3(input.held[i])));
  }
}

TEST_CASE("the probe runs from the eye past the far end of the held box") {
  const ThrowInput input = Rifle();
  const ThrowProbe probe = MakeThrowProbe(input);
  CHECK(Near(probe.origin, input.eye));
  CHECK(glm::length(probe.direction) == doctest::Approx(1.0f));
  const glm::vec3 end = probe.origin + probe.direction * probe.length;
  // The muzzle end is about 1.05 m ahead of the eye.
  CHECK(-end.z > 1.0f);
  CHECK(-end.z < 1.1f);
  // An obstacle beyond the probe does not block.
  CHECK_FALSE(PlanThrow(input, {}, probe.length + 0.1f).blocked);
}

TEST_CASE("a blocked throw is pulled back from the obstacle and just dropped") {
  const ThrowInput input = Rifle();
  const ThrowTuning tuning;
  const ThrowProbe probe = MakeThrowProbe(input);
  const float wall = probe.length - 0.3f;
  const ThrowPlan plan = PlanThrow(input, tuning, wall);
  CHECK(plan.blocked);
  const glm::vec3 moved = plan.position - glm::vec3(input.held[3]);
  CHECK(Near(moved, -probe.direction * (0.3f + tuning.clearance)));
  CHECK(Near(plan.linear_velocity, input.player_velocity));
  CHECK(Near(plan.angular_velocity, glm::vec3(0.0f)));
}

TEST_CASE("a throw against a wall at the eye puts the box centre on the eye") {
  const ThrowInput input = Rifle();
  const ThrowPlan plan = PlanThrow(input, {}, 0.0f);
  CHECK(plan.blocked);
  const glm::vec3 center =
      glm::vec3(PlanMatrix(plan) * glm::vec4(input.bounds.center(), 1.0f));
  const ThrowProbe probe = MakeThrowProbe(input);
  CHECK(glm::dot(center - input.eye, probe.direction) ==
        doctest::Approx(0.0f).epsilon(1.0e-4));
}

TEST_CASE("a scaled model keeps its scale") {
  ThrowInput input = Rifle();
  input.held = glm::scale(input.held, glm::vec3(2.0f));
  const ThrowPlan plan = PlanThrow(input, {}, std::nullopt);
  CHECK(Near(plan.scale, glm::vec3(2.0f)));
  const glm::mat4 matrix = PlanMatrix(plan);
  for (int i = 0; i < 4; ++i) {
    CHECK(Near(glm::vec3(matrix[i]), glm::vec3(input.held[i])));
  }
}

TEST_CASE("the swap toss is gentler than a throw") {
  const ThrowTuning toss = SwapTossTuning();
  const ThrowTuning thrown;
  CHECK(toss.speed < thrown.speed);
  CHECK(toss.lift <= thrown.lift);
}

}  // namespace
}  // namespace blue::weapon

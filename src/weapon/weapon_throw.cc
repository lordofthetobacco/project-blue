#include "weapon/weapon_throw.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace blue::weapon {
namespace {

constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
constexpr float kMinLength = 1.0e-6f;

// World-space centre of the held box.
glm::vec3 HeldCenter(const ThrowInput& input) {
  return glm::vec3(input.held * glm::vec4(input.bounds.center(), 1.0f));
}

}  // namespace

ThrowTuning SwapTossTuning() {
  return {.speed = 1.5f, .lift = 0.5f, .spin = 1.0f};
}

ThrowProbe MakeThrowProbe(const ThrowInput& input) {
  // The box's extent along the view: the sum of its (scaled) half axes
  // projected on it.
  const glm::vec3 half = 0.5f * input.bounds.size();
  float extent = 0.0f;
  for (int i = 0; i < 3; ++i) {
    extent += std::abs(glm::dot(input.forward, glm::vec3(input.held[i]))) *
              std::abs(half[i]);
  }
  const glm::vec3 far_side = HeldCenter(input) + input.forward * extent;
  const glm::vec3 to_far_side = far_side - input.eye;
  const float length = glm::length(to_far_side);
  if (!(length > kMinLength)) {
    return {.origin = input.eye, .direction = input.forward, .length = 0.0f};
  }
  return {
      .origin = input.eye, .direction = to_far_side / length, .length = length};
}

ThrowPlan PlanThrow(const ThrowInput& input, const ThrowTuning& tuning,
                    std::optional<float> blocked_at) {
  ThrowPlan plan;
  plan.position = glm::vec3(input.held[3]);
  glm::mat3 rotation(1.0f);
  for (int i = 0; i < 3; ++i) {
    const glm::vec3 axis(input.held[i]);
    const float length = glm::length(axis);
    plan.scale[i] = length > kMinLength ? length : 1.0f;
    if (length > kMinLength) {
      rotation[i] = axis / length;
    }
  }
  plan.rotation = glm::normalize(glm::quat_cast(rotation));

  const ThrowProbe probe = MakeThrowProbe(input);
  if (blocked_at.has_value() && *blocked_at < probe.length) {
    // Back towards the eye, but the centre no further back than the eye.
    const float wanted =
        probe.length - std::max(*blocked_at, 0.0f) + tuning.clearance;
    const float limit = std::max(
        glm::dot(HeldCenter(input) - input.eye, probe.direction), 0.0f);
    plan.position -= probe.direction * std::min(wanted, limit);
    plan.linear_velocity = input.player_velocity;
    plan.blocked = true;
    return plan;
  }
  plan.linear_velocity =
      input.player_velocity + input.forward * tuning.speed + kUp * tuning.lift;
  plan.angular_velocity = kUp * tuning.spin;
  return plan;
}

glm::mat4 PlanMatrix(const ThrowPlan& plan) {
  glm::mat4 matrix = glm::mat4_cast(plan.rotation);
  for (int i = 0; i < 3; ++i) {
    matrix[i] *= plan.scale[i];
  }
  matrix[3] = glm::vec4(plan.position, 1.0f);
  return matrix;
}

}  // namespace blue::weapon

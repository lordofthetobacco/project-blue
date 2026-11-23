#include "weapon/dispersion.h"

#include <cmath>
#include <cstdint>
#include <numbers>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace blue::weapon {
namespace {

constexpr uint32_t kFallbackSeed = 0x5eed5u;

}  // namespace

Dispersion::Dispersion(uint32_t seed)
    : state_(seed != 0 ? seed : kFallbackSeed) {}

glm::vec3 Dispersion::Apply(glm::vec3 direction, float accuracy_moa) {
  if (!(accuracy_moa > 0.0f) || !std::isfinite(accuracy_moa)) {
    return direction;
  }
  const float length = glm::length(direction);
  if (!(length > 0.0f)) {
    return direction;
  }
  const glm::vec3 forward = direction / length;
  // Any two unit vectors perpendicular to the bore span the cone's disc.
  const glm::vec3 helper = std::abs(forward.y) < 0.99f
                               ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 right = glm::normalize(glm::cross(forward, helper));
  const glm::vec3 up = glm::cross(right, forward);

  // Uniform over the disc: the radius goes with the square root.
  const float max_angle = 0.5f * accuracy_moa * kRadiansPerMoa;
  const float angle = max_angle * std::sqrt(NextUnit());
  const float turn = 2.0f * std::numbers::pi_v<float> * NextUnit();
  const glm::vec3 offset =
      std::tan(angle) * (std::cos(turn) * right + std::sin(turn) * up);
  return glm::normalize(forward + offset) * length;
}

float Dispersion::NextUnit() {
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  constexpr float kToUnit = 1.0f / 4294967296.0f;
  // The float product can round up to 1 for the largest states.
  const float unit = static_cast<float>(state_) * kToUnit;
  return unit < 1.0f ? unit : 0.99999994f;
}

}  // namespace blue::weapon

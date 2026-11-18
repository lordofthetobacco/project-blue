#include "scene/scene.h"

#include <cmath>
#include <optional>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace blue::scene {

std::optional<std::string_view> ValidateRigidBody(const RigidBody& body) {
  if (!std::isfinite(body.mass) || !std::isfinite(body.friction) ||
      !std::isfinite(body.restitution)) {
    return "non-finite value";
  }
  if (body.motion == MotionType::kDynamic && body.mass <= 0.0f) {
    return "dynamic body needs mass > 0";
  }
  if (body.friction < 0.0f) {
    return "friction must be >= 0";
  }
  if (body.restitution < 0.0f || body.restitution > 1.0f) {
    return "restitution must be in [0, 1]";
  }
  if (body.shape == ShapeType::kTriangleMesh &&
      body.motion != MotionType::kStatic) {
    return "triangle mesh shape requires a static body";
  }
  return std::nullopt;
}

std::optional<std::string_view> ValidateSunLight(const SunLight& sun) {
  for (const glm::vec3& v : {sun.direction, sun.color}) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
      return "non-finite value";
    }
  }
  if (!std::isfinite(sun.intensity)) {
    return "non-finite value";
  }
  if (glm::length(sun.direction) <= 1e-6f) {
    return "direction must be non-zero";
  }
  if (sun.color.x < 0.0f || sun.color.y < 0.0f || sun.color.z < 0.0f) {
    return "color must be >= 0";
  }
  if (sun.intensity < 0.0f) {
    return "intensity must be >= 0";
  }
  return std::nullopt;
}

std::optional<std::string_view> ValidateEnvironment(
    const Environment& environment) {
  if (!std::isfinite(environment.intensity) ||
      !std::isfinite(environment.rotation_radians) ||
      !std::isfinite(environment.exposure_ev)) {
    return "non-finite value";
  }
  if (environment.intensity < 0.0f) {
    return "intensity must be >= 0";
  }
  return std::nullopt;
}

glm::mat4 Transform::Matrix() const {
  return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
         glm::scale(glm::mat4(1.0f), scale);
}

}  // namespace blue::scene

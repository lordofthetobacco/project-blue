#include "camera/look.h"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

namespace blue::camera {
namespace {

constexpr float kMaxPitch = glm::radians(89.0f);
constexpr float kMinDirectionLength = 1.0e-6f;

}  // namespace

float WrapYaw(float yaw) {
  // remainder() maps to [-pi, pi]; the interval must be half-open at -pi.
  const float wrapped = std::remainder(yaw, glm::two_pi<float>());
  return wrapped <= -glm::pi<float>() ? glm::pi<float>() : wrapped;
}

glm::vec3 ForwardFrom(LookAngles look) {
  const float cos_pitch = std::cos(look.pitch);
  return {-std::sin(look.yaw) * cos_pitch, std::sin(look.pitch),
          -std::cos(look.yaw) * cos_pitch};
}

glm::vec3 FlatRightFrom(float yaw) {
  return {std::cos(yaw), 0.0f, -std::sin(yaw)};
}

glm::vec3 FlatForwardFrom(float yaw) {
  return {-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

LookAngles ApplyMouseLook(LookAngles look, glm::vec2 delta,
                          const input::InputSettings& settings) {
  const float vertical_sign = settings.invert_y ? 1.0f : -1.0f;
  return {
      .yaw = WrapYaw(look.yaw - delta.x * settings.mouse_sensitivity),
      .pitch = std::clamp(
          look.pitch + vertical_sign * delta.y * settings.mouse_sensitivity,
          -kMaxPitch, kMaxPitch),
  };
}

LookAngles LookTowards(glm::vec3 from, glm::vec3 to) {
  const glm::vec3 offset = to - from;
  const float distance = glm::length(offset);
  if (!std::isfinite(distance) || distance < kMinDirectionLength) {
    return {};
  }
  const glm::vec3 direction = offset / distance;
  return {
      .yaw = std::atan2(-direction.x, -direction.z),
      .pitch = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)),
                          -kMaxPitch, kMaxPitch),
  };
}

}  // namespace blue::camera

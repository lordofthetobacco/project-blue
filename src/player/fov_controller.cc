#include "player/fov_controller.h"

#include <algorithm>
#include <cmath>

#include <glm/trigonometric.hpp>

#include "input/input_settings.h"

namespace blue::player {

float MagnifiedFovDegrees(float base_degrees, float magnification) {
  const float half = glm::radians(base_degrees) * 0.5f;
  return glm::degrees(
      2.0f * std::atan(std::tan(half) / std::max(magnification, 1.0f)));
}

float ZoomSensitivityScale(float fov_degrees, float base_fov_degrees) {
  const float tan_base = std::tan(glm::radians(base_fov_degrees) * 0.5f);
  if (!(tan_base > 0.0f)) {
    return 1.0f;
  }
  return std::tan(glm::radians(fov_degrees) * 0.5f) / tan_base;
}

float FovController::Update(const FovInput& input) {
  const float boost = input.sprinting ? input.sprint_boost_degrees : 0.0f;
  const float target =
      std::clamp(input.base_degrees + boost, input::kMinFovDegrees,
                 input::kMaxFovDegrees + input::kMaxSprintFovBoostDegrees);
  if (!initialized_) {
    current_ = target;
    initialized_ = true;
  } else {
    const float dt = std::max(input.dt, 0.0f);
    current_ += (target - current_) * (1.0f - std::exp(-input.blend_rate * dt));
  }
  return current_ * input.zoom;
}

}  // namespace blue::player

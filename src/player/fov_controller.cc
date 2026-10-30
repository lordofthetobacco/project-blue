#include "player/fov_controller.h"

#include <algorithm>
#include <cmath>

#include "input/input_settings.h"

namespace blue::player {

float FovController::Update(const FovInput& input) {
  const float boost = input.sprinting ? input.sprint_boost_degrees : 0.0f;
  const float target =
      std::clamp(input.base_degrees + boost, input::kMinFovDegrees,
                 input::kMaxFovDegrees + input::kMaxSprintFovBoostDegrees);
  if (!initialized_) {
    current_ = target;
    initialized_ = true;
    return current_;
  }
  const float dt = std::max(input.dt, 0.0f);
  current_ += (target - current_) * (1.0f - std::exp(-input.blend_rate * dt));
  return current_;
}

}  // namespace blue::player

#include "player/head_bob.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

namespace blue::player {
namespace {

constexpr float kMinWeight = 1.0e-3f;

float FrequencyFor(const HeadBobParams& params, Gait gait) {
  switch (gait) {
    case Gait::kWalk:
      return params.walk_frequency_hz;
    case Gait::kSprint:
      return params.sprint_frequency_hz;
    case Gait::kCrouch:
      return params.crouch_frequency_hz;
  }
  return params.walk_frequency_hz;
}

}  // namespace

HeadBobOffset HeadBob::Update(const HeadBobInput& input) {
  const float dt = std::max(input.dt, 0.0f);
  const bool stepping =
      input.grounded && input.horizontal_speed > params_.min_speed;
  const float target = stepping ? 1.0f : 0.0f;
  weight_ += (target - weight_) * (1.0f - std::exp(-params_.fade_rate * dt));

  if (weight_ > kMinWeight) {
    phase_ += glm::two_pi<float>() * FrequencyFor(params_, input.gait) * dt;
  }

  const float speed_scale =
      params_.reference_speed > 0.0f
          ? std::min(input.horizontal_speed / params_.reference_speed,
                     params_.max_speed_scale)
          : 1.0f;
  const float scale =
      std::clamp(input.intensity, 0.0f, 1.0f) * weight_ * speed_scale;
  const float sway = std::sin(0.5f * phase_);
  return {
      .local_offset = {params_.lateral_amplitude * scale * sway,
                       params_.vertical_amplitude * scale * std::sin(phase_),
                       0.0f},
      .roll_radians = glm::radians(params_.roll_degrees) * scale * sway,
  };
}

void HeadBob::Reset() {
  phase_ = 0.0f;
  weight_ = 0.0f;
}

}  // namespace blue::player

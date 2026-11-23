#include "weapon/scope.h"

#include <algorithm>
#include <cmath>

namespace blue::weapon {
namespace {

// Time constants in `zoom_time`: e^-3 leaves 5 % of a change.
constexpr float kBlendTimeConstants = 3.0f;
// The shown magnification snaps to the chosen one this close (relative).
constexpr float kSnapRatio = 1.0e-4f;

}  // namespace

float ClampMagnification(const ScopeSpec& spec, float magnification) {
  const float low = std::max(spec.min_magnification, 1.0f);
  const float high = std::max(spec.max_magnification, low);
  return std::clamp(magnification, low, high);
}

float ClampZero(const ScopeSpec& spec, float distance) {
  const float low = std::max(spec.min_zero, 1.0f);
  const float high = std::max(spec.max_zero, low);
  return std::clamp(distance, low, high);
}

float StepZero(const ScopeSpec& spec, float distance, int steps) {
  return ClampZero(spec, ClampZero(spec, distance) +
                             static_cast<float>(steps) * spec.zero_step);
}

ScopeZoom::ScopeZoom(const ScopeSpec& spec, float magnification)
    : spec_(spec),
      target_(ClampMagnification(spec, magnification)),
      current_(target_) {}

bool ScopeZoom::AddWheel(float notches) {
  if (!std::isfinite(notches)) {
    return false;
  }
  pending_notches_ += notches;
  const float whole = std::trunc(pending_notches_);
  if (whole == 0.0f) {
    return false;
  }
  pending_notches_ -= whole;
  const float before = target_;
  target_ = ClampMagnification(spec_, target_ + whole * spec_.step);
  return target_ != before;
}

void ScopeZoom::Update(float dt) {
  const float rate = kBlendTimeConstants / std::max(spec_.zoom_time, 1.0e-3f);
  const float blend = 1.0f - std::exp(-rate * std::max(dt, 0.0f));
  current_ *= std::pow(target_ / current_, blend);
  if (std::abs(current_ / target_ - 1.0f) < kSnapRatio) {
    current_ = target_;
  }
}

}  // namespace blue::weapon

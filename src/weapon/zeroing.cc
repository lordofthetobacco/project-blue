#include "weapon/zeroing.h"

#include <cmath>
#include <limits>

#include <glm/vec3.hpp>

#include "core/det_math.h"
#include "weapon/projectile_system.h"

namespace blue::weapon {
namespace {

constexpr float kMaxFlightSeconds = 10.0f;
// Bisection on the angle: the bracket starts a few mrad wide, so this many
// halvings leave far less than a micro-radian.
constexpr int kBisections = 40;
// Bracket of the search around the drag-free angle, radians: drag only ever
// asks for more elevation, and never 0.2 rad more at rifle ranges.
constexpr float kBelowDragFree = 1.0e-3f;
constexpr float kMaxExtraElevation = 0.2f;

// Returns tan of the angle above the line of sight at which a drag-free
// bullet leaving `height` m below the line and `range` m short of the zero
// crosses the line at the zero; `speed` m/s, `gravity` m/s^2. Solves
// -h + L t - k (1 + t^2) = 0 with k = g L^2 / (2 v^2), smaller root.
float DragFreeZeroTangent(float height, float range, float speed,
                          float gravity) {
  if (!(range > 0.0f)) {
    return 0.0f;
  }
  const float k =
      speed > 0.0f ? 0.5f * gravity * range * range / (speed * speed) : 0.0f;
  if (k <= 1.0e-9f) {
    return height / range;
  }
  const float discriminant = range * range - 4.0f * k * (height + k);
  if (discriminant < 0.0f) {
    return range / (2.0f * k);  // out of reach: the 45 degree-ish maximum
  }
  // Stable form of the smaller root (L - sqrt(D)) / 2k: no cancellation.
  return 2.0f * (height + k) / (range + std::sqrt(discriminant));
}

}  // namespace

float HeightAtDistance(const ZeroSetup& setup, float angle, float distance) {
  if (!(setup.muzzle_velocity > 0.0f)) {
    return -std::numeric_limits<float>::infinity();
  }
  // x downrange, y up; the line of sight is the x axis.
  const glm::vec3 gravity(0.0f, -setup.gravity, 0.0f);
  glm::vec3 position(setup.muzzle_ahead, -setup.muzzle_below, 0.0f);
  // Deterministic trigonometry: the zero angle feeds simulated shots.
  const core::SinCos elevation = core::DetSinCos(angle);
  glm::vec3 velocity =
      setup.muzzle_velocity * glm::vec3(elevation.cos, elevation.sin, 0.0f);
  if (position.x >= distance) {
    return position.y;
  }
  const int max_steps = static_cast<int>(kMaxFlightSeconds / kZeroStepSeconds);
  for (int step = 0; step < max_steps; ++step) {
    const BallisticStep next =
        IntegrateBallisticStep(velocity, gravity, setup.drag, kZeroStepSeconds);
    const glm::vec3 end = position + next.displacement;
    if (end.x >= distance) {
      const float t = (distance - position.x) / (end.x - position.x);
      return position.y + t * (end.y - position.y);
    }
    if (!(next.velocity.x > 0.0f)) {
      break;  // stalled: never gets there
    }
    position = end;
    velocity = next.velocity;
  }
  return -std::numeric_limits<float>::infinity();
}

float ZeroAngle(const ZeroSetup& setup, float distance) {
  if (!(distance > 0.0f) || !(setup.muzzle_velocity > 0.0f)) {
    return 0.0f;
  }
  const float drag_free = core::DetAtan(
      DragFreeZeroTangent(setup.muzzle_below, distance - setup.muzzle_ahead,
                          setup.muzzle_velocity, setup.gravity));
  if (!(setup.drag > 0.0f)) {
    return drag_free;
  }
  // The height at the zero grows with the angle on the flat branch: bisect.
  float low = drag_free - kBelowDragFree;
  float high = drag_free + kMaxExtraElevation;
  if (HeightAtDistance(setup, high, distance) < 0.0f) {
    return high;  // out of reach: the most this search offers
  }
  for (int i = 0; i < kBisections; ++i) {
    const float middle = 0.5f * (low + high);
    if (HeightAtDistance(setup, middle, distance) < 0.0f) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return 0.5f * (low + high);
}

}  // namespace blue::weapon

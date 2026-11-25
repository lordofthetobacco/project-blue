#include "sim/tick_clock.h"

#include <algorithm>
#include <cmath>

#include "physics/physics_world.h"
#include "sim/user_command.h"

namespace blue::sim {
namespace {

// The tick is the physics step: one tick runs exactly one PhysicsWorld step.
static_assert(kTickSeconds == physics::kFixedStepSeconds);

constexpr double kTick = 1.0 / 60.0;
static_assert(static_cast<float>(kTick) == kTickSeconds);
constexpr double kMaxFrameSeconds = 0.25;
// Absorbs float rounding so e.g. 60 calls of 1/60 s yield exactly 60 ticks.
constexpr double kTickEpsilon = 1.0e-6;

float AlphaOf(double accumulator) {
  return static_cast<float>(std::clamp(accumulator / kTick, 0.0, 1.0));
}

}  // namespace

TickAdvance TickClock::Advance(float frame_seconds) {
  if (!(frame_seconds > 0.0f)) {  // Also rejects NaN.
    return TickAdvance{.ticks = 0, .alpha = AlphaOf(accumulator_)};
  }
  accumulator_ += std::min(static_cast<double>(frame_seconds) * time_scale_,
                           kMaxFrameSeconds);
  const int due = static_cast<int>(
      std::floor((accumulator_ + kTickEpsilon * kTick) / kTick));
  if (due <= 0) {
    return TickAdvance{.ticks = 0, .alpha = AlphaOf(accumulator_)};
  }
  const int ticks = std::min(due, kMaxTicksPerAdvance);
  accumulator_ = std::max(0.0, accumulator_ - ticks * kTick);
  if (due > kMaxTicksPerAdvance) {
    // Drop the backlog instead of spiraling; keep only the sub-tick remainder.
    accumulator_ = std::fmod(accumulator_, kTick);
  }
  return TickAdvance{.ticks = ticks, .alpha = AlphaOf(accumulator_)};
}

void TickClock::SetTimeScale(float scale) {
  time_scale_ = std::isfinite(scale)
                    ? std::clamp(static_cast<double>(scale),
                                 static_cast<double>(kMinTimeScale),
                                 static_cast<double>(kMaxTimeScale))
                    : 1.0;
}

float TickClock::time_scale() const { return static_cast<float>(time_scale_); }

void TickClock::Reset() { accumulator_ = 0.0; }

}  // namespace blue::sim

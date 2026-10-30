#include "player/head_bob.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace blue::player {

namespace {

constexpr float kDt = 1.0f / 120.0f;

HeadBobInput Walking(float intensity = 1.0f, Gait gait = Gait::kWalk,
                     bool grounded = true, float speed = 4.0f) {
  return {.horizontal_speed = speed,
          .gait = gait,
          .grounded = grounded,
          .intensity = intensity,
          .dt = kDt};
}

// Runs `seconds` of `input`, returning the last offset.
HeadBobOffset Run(HeadBob& bob, const HeadBobInput& input, float seconds) {
  HeadBobOffset last;
  const int steps = static_cast<int>(std::round(seconds / kDt));
  for (int i = 0; i < steps; ++i) {
    last = bob.Update(input);
  }
  return last;
}

// Returns the largest |vertical offset| over `seconds` of `input`.
float PeakVertical(HeadBob& bob, const HeadBobInput& input, float seconds) {
  float peak = 0.0f;
  const int steps = static_cast<int>(std::round(seconds / kDt));
  for (int i = 0; i < steps; ++i) {
    peak = std::max(peak, std::abs(bob.Update(input).local_offset.y));
  }
  return peak;
}

}  // namespace

TEST_CASE("Zero intensity produces no offset while walking") {
  HeadBob bob;
  const HeadBobOffset offset = Run(bob, Walking(0.0f), 2.0f);
  CHECK(offset.local_offset == glm::vec3(0.0f));
  CHECK(offset.roll_radians == 0.0f);
  CHECK(bob.phase() > 0.0f);  // the rhythm still runs, only the output is muted
}

TEST_CASE("Standing still after walking fades the bob out") {
  HeadBob bob;
  (void)Run(bob, Walking(), 2.0f);
  HeadBobInput standing = Walking();
  standing.horizontal_speed = 0.0f;
  const HeadBobOffset offset = Run(bob, standing, 1.0f);
  CHECK(glm::length(offset.local_offset) < 1.0e-4f);
  CHECK(std::abs(offset.roll_radians) < 1.0e-4f);
}

TEST_CASE("A fresh bob is at rest") {
  HeadBob bob;
  HeadBobInput standing = Walking();
  standing.horizontal_speed = 0.0f;
  CHECK(bob.Update(standing).local_offset == glm::vec3(0.0f));
}

TEST_CASE("Walking at constant speed is periodic at the walk frequency") {
  HeadBob bob;
  const HeadBobParams params = bob.params();
  (void)Run(bob, Walking(), 3.0f);  // weight has converged
  const HeadBobOffset a = Run(bob, Walking(), kDt);
  const HeadBobOffset b = Run(bob, Walking(), 1.0f / params.walk_frequency_hz);
  CHECK(a.local_offset.y == doctest::Approx(b.local_offset.y).epsilon(0.01));
  // The sway has half the frequency: one full period takes two steps.
  const HeadBobOffset c = Run(bob, Walking(), 1.0f / params.walk_frequency_hz);
  CHECK(a.local_offset.x == doctest::Approx(c.local_offset.x).epsilon(0.01));
  CHECK(a.roll_radians == doctest::Approx(c.roll_radians).epsilon(0.01));
}

TEST_CASE("Sprinting bobs harder and faster than walking, crouching slower") {
  HeadBob walk;
  HeadBob sprint;
  HeadBob crouch;
  const float walk_peak = PeakVertical(walk, Walking(), 3.0f);
  const float sprint_peak =
      PeakVertical(sprint, Walking(1.0f, Gait::kSprint, true, 7.0f), 3.0f);
  (void)PeakVertical(crouch, Walking(1.0f, Gait::kCrouch, true, 2.0f), 3.0f);
  CHECK(walk_peak > 0.02f);
  CHECK(sprint_peak > walk_peak);
  CHECK(sprint.phase() > walk.phase());
  CHECK(crouch.phase() < walk.phase());
}

TEST_CASE("Amplitude follows speed up to the cap") {
  HeadBob slow;
  HeadBob fast;
  HeadBob faster;
  const float slow_peak =
      PeakVertical(slow, Walking(1.0f, Gait::kWalk, true, 2.0f), 3.0f);
  const float fast_peak =
      PeakVertical(fast, Walking(1.0f, Gait::kWalk, true, 6.0f), 3.0f);
  const float faster_peak =
      PeakVertical(faster, Walking(1.0f, Gait::kWalk, true, 60.0f), 3.0f);
  CHECK(slow_peak < fast_peak);
  CHECK(faster_peak == doctest::Approx(fast_peak).epsilon(0.02));
}

TEST_CASE("Leaving the ground fades the bob out within a second") {
  HeadBob bob;
  (void)Run(bob, Walking(), 2.0f);
  const HeadBobOffset airborne =
      Run(bob, Walking(1.0f, Gait::kWalk, false), 1.0f);
  CHECK(glm::length(airborne.local_offset) < 1.0e-3f);
}

TEST_CASE("Reset returns to rest instantly") {
  HeadBob bob;
  (void)Run(bob, Walking(), 1.0f);
  bob.Reset();
  CHECK(bob.phase() == 0.0f);
  HeadBobInput standing = Walking();
  standing.horizontal_speed = 0.0f;
  CHECK(bob.Update(standing).local_offset == glm::vec3(0.0f));
}

}  // namespace blue::player

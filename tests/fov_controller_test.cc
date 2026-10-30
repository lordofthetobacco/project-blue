#include "player/fov_controller.h"

#include <doctest/doctest.h>

#include <cmath>

#include "input/input_settings.h"

namespace blue::player {

namespace {

constexpr float kDt = 1.0f / 60.0f;

FovInput Input(bool sprinting, float base = 70.0f, float boost = 10.0f) {
  return {.base_degrees = base,
          .sprint_boost_degrees = boost,
          .sprinting = sprinting,
          .blend_rate = 8.0f,
          .dt = kDt};
}

float Run(FovController& fov, const FovInput& input, float seconds) {
  float last = 0.0f;
  const int steps = static_cast<int>(std::round(seconds / kDt));
  for (int i = 0; i < steps; ++i) {
    last = fov.Update(input);
  }
  return last;
}

}  // namespace

TEST_CASE("The first update snaps to the target") {
  FovController fov;
  CHECK(fov.Update(Input(false, 85.0f)) == 85.0f);
  FovController sprinting;
  CHECK(sprinting.Update(Input(true, 85.0f, 10.0f)) == 95.0f);
}

TEST_CASE("Sprinting converges to base plus boost without overshoot") {
  FovController fov;
  (void)fov.Update(Input(false));
  float previous = 70.0f;
  for (int i = 0; i < 120; ++i) {
    const float current = fov.Update(Input(true));
    CHECK(current >= previous - 1.0e-4f);
    CHECK(current <= 80.0f + 1.0e-4f);
    previous = current;
  }
  CHECK(previous == doctest::Approx(80.0f).epsilon(0.001));
  CHECK(Run(fov, Input(false), 2.0f) == doctest::Approx(70.0f).epsilon(0.001));
}

TEST_CASE("A change of the base setting is tracked") {
  FovController fov;
  (void)fov.Update(Input(false, 70.0f));
  CHECK(Run(fov, Input(false, 100.0f), 2.0f) ==
        doctest::Approx(100.0f).epsilon(0.001));
}

TEST_CASE("The target is clamped to the settings range") {
  FovController fov;
  CHECK(fov.Update(Input(false, 10.0f)) == input::kMinFovDegrees);
  fov.Reset();
  CHECK(fov.Update(Input(true, 1000.0f, 1000.0f)) ==
        input::kMaxFovDegrees + input::kMaxSprintFovBoostDegrees);
}

TEST_CASE("Reset makes the next update snap again") {
  FovController fov;
  (void)fov.Update(Input(false, 70.0f));
  fov.Reset();
  CHECK(fov.Update(Input(false, 90.0f)) == 90.0f);
}

}  // namespace blue::player

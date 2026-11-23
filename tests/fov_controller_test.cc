#include "player/fov_controller.h"

#include <doctest/doctest.h>

#include <cmath>

#include <glm/trigonometric.hpp>

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

TEST_CASE("Zoom scales the result without disturbing the smoothed value") {
  FovController fov;
  FovInput aimed = Input(false, 70.0f);
  aimed.zoom = 0.8f;
  CHECK(fov.Update(aimed) == doctest::Approx(56.0f));
  CHECK(fov.current() == 70.0f);
  // The zoom applies after the clamp, so it can go below the settings range.
  fov.Reset();
  FovInput narrow = Input(false, input::kMinFovDegrees);
  narrow.zoom = 0.5f;
  CHECK(fov.Update(narrow) == doctest::Approx(input::kMinFovDegrees * 0.5f));
}

TEST_CASE("Reset makes the next update snap again") {
  FovController fov;
  (void)fov.Update(Input(false, 70.0f));
  fov.Reset();
  CHECK(fov.Update(Input(false, 90.0f)) == 90.0f);
}

TEST_CASE("Magnification M narrows the FOV to 2 atan(tan(base / 2) / M)") {
  CHECK(MagnifiedFovDegrees(70.0f, 1.0f) == doctest::Approx(70.0f));
  CHECK(MagnifiedFovDegrees(70.0f, 0.5f) == doctest::Approx(70.0f));
  for (const float m : {2.0f, 3.5f, 10.0f}) {
    const float fov = MagnifiedFovDegrees(70.0f, m);
    // The picture's linear size grows by exactly M.
    CHECK(std::tan(glm::radians(fov) * 0.5f) ==
          doctest::Approx(std::tan(glm::radians(35.0f)) / m));
  }
  // Not the linear fraction of the angle: 10x of 70 degrees is 7.99.
  CHECK(MagnifiedFovDegrees(70.0f, 10.0f) ==
        doctest::Approx(7.99f).epsilon(0.01));
}

TEST_CASE("The zoom sensitivity scale is the half-angle tangent ratio") {
  CHECK(ZoomSensitivityScale(70.0f, 70.0f) == doctest::Approx(1.0f));
  CHECK(ZoomSensitivityScale(MagnifiedFovDegrees(70.0f, 6.0f), 70.0f) ==
        doctest::Approx(1.0f / 6.0f));
  CHECK(ZoomSensitivityScale(50.0f, 0.0f) == 1.0f);  // No base yet.
}

}  // namespace blue::player
